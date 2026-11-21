//
// Created by josef on 06.01.17.
//

#include "queryCompiler/queryCompiler.hpp"

#include <algorithm>
#include <mutex>
#include <exception>
#include <optional>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Support/raw_ostream.h>
#include "codegen/LLVMCompat.hpp"
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Target/TargetMachine.h>
#include <include/tardisdb/sqlParser/SQLParser.hpp>

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "queryExecutor/queryExecutor.hpp"

#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"
#include "native/sql/SqlTuple.hpp"
#include "native/sql/SqlValues.hpp"
#include "sqlParser/Expression.hpp"
#include "sqlParser/ParserResult.hpp"

#if USE_HYRISE
    #include "SQLParser.h"
#endif

namespace QueryCompiler {

    Latching latching = Latching::Branch;

    namespace {

    std::mutex serialLatch;

    /// Confines a statement of a transaction to its snapshot: the session's branch is read and written
    /// in the snapshot. VERSION ALL, DESCENDANTS and LEAVES select committed branches only, as they are
    /// when the statement runs. Statements that change the catalog would survive a rollback, and writes
    /// of other branches would not be merged at commit: neither runs.
    void confine(semanticalAnalysis::SQLParserResult & statement, const Transaction & transaction, Database & db) {
        using OpType = semanticalAnalysis::SQLParserResult::OpType;
        auto redirect = [&](std::string & version) {
            auto it = db._branchMapping.find(version); // main is also called master
            auto session = db._branchMapping.find(transaction.branch);
            if (it != db._branchMapping.end() && session != db._branchMapping.end() && it->second == session->second) {
                version = transaction.snapshot;
            }
        };
        auto write = [&](semanticalAnalysis::Relation & relation, const std::string & tag) {
            redirect(relation.version);
            if (relation.version != transaction.snapshot) {
                throw TransactionError("25001", tag + " of branch " + relation.version + " cannot run inside a transaction of branch " + transaction.branch);
            }
        };
        switch (statement.opType) {
            case OpType::Select:
                for (auto & relation : statement.selectStmt->relations) {
                    // the temporary tables of the statement (WITH, SELECT without FROM) are read in master
                    if (relation.name == sqlExpressions::oneRowTable || TemporaryTables::find(db, relation.name) != nullptr) continue;
                    if (relation.versionScope.empty() || relation.versionScope == "set" || relation.versionScope == "diff") {
                        redirect(relation.version);
                        for (auto & version : relation.versionSet) redirect(version);
                    }
                }
                break;
            case OpType::Insert: write(statement.insertStmt->relation, "INSERT"); break;
            case OpType::Update: write(statement.updateStmt->relation, "UPDATE"); break;
            case OpType::Delete: write(statement.deleteStmt->relation, "DELETE"); break;
            default:
                throw TransactionError("25001", "only SELECT, INSERT, UPDATE and DELETE run inside a transaction");
        }
    }

    /// The latches of a statement, held from its analysis to the end of its execution. Statements that
    /// change tables or branches hold the catalog latch exclusive; all others hold it shared and latch
    /// the tables they access in the order of their addresses, shared for reading and exclusive for
    /// writing. With Latching::Branch, updates and deletes in a branch other than main hold the table
    /// shared and their branch exclusive, and reads hold their branches shared (after the analysis has
    /// found them, in the order of their ids): a branch's writes change nothing another branch reads.
    /// Updates of main, and with eager membership its deletes, hold the table shared and main exclusive
    /// too; updates complete their in-place writes at the end (InPlaceWrites), and the other statements
    /// that hold a table shared enter its ReaderEpochs once they hold all their latches, so that those
    /// writes can wait for them. Inserts, one at a time per table (Table::_insertLatch), hold the table
    /// shared and their branch exclusive if the tuple fits into the storage without moving it
    /// (Table::capacity()); otherwise they hold the table exclusive and the storage grows. The latches prefer writers (SharedLatch), so that statements which
    /// keep overlapping do not starve one that changes the catalog.
    class StatementLatches {
    public:
        StatementLatches(Database & db, semanticalAnalysis::SQLParserResult & statement, const Transaction * transaction = nullptr) {
            using OpType = semanticalAnalysis::SQLParserResult::OpType;
            // a transaction's statement is confined to its snapshot under the latches, since BEGIN changes the catalog
            if (latching == Latching::Serial) {
                _serial = std::unique_lock<std::mutex>(serialLatch);
                if (transaction != nullptr) confine(statement, *transaction, db);
                return;
            }
            std::vector<std::string> read, written;
            const semanticalAnalysis::Relation * branchWrite = nullptr; // updated or deleted in
            const semanticalAnalysis::Relation * insertInto = nullptr;
            switch (statement.opType) {
                case OpType::Select:
                    for (auto & relation : statement.selectStmt->relations) read.push_back(relation.name);
                    _readsBranches = (latching == Latching::Branch);
                    break;
                case OpType::Insert: insertInto = &statement.insertStmt->relation; break;
                case OpType::Update: branchWrite = &statement.updateStmt->relation; _writesInPlace = true; break;
#if LAZY_MEMBERSHIP
                // a delete adds an entry to the membership of the table, which all branches read
                case OpType::Delete: written.push_back(statement.deleteStmt->relation.name); break;
#else
                // a delete in main changes only the bitmap column of main
                case OpType::Delete: branchWrite = &statement.deleteStmt->relation; break;
#endif
                default:
                    _exclusiveCatalog = std::unique_lock<SharedLatch>(db._catalogLatch);
                    if (transaction != nullptr) confine(statement, *transaction, db);
                    return;
            }
            _sharedCatalog = std::shared_lock<SharedLatch>(db._catalogLatch);
            if (transaction != nullptr) confine(statement, *transaction, db);

            Branch * writtenBranch = nullptr;
            if (insertInto != nullptr) {
                Table * table = db.getTable(insertInto->name);
                auto it = db._branchMapping.find(insertInto->version);
                if (latching == Latching::Branch && table != nullptr && it != db._branchMapping.end()) {
                    // one insert at a time; it decides whether the tuple fits without moving the storage
                    _insertLatch = std::unique_lock<std::mutex>(table->_insertLatch);
                    if (table->size() < table->capacity()) {
                        read.push_back(insertInto->name);
                        writtenBranch = db._branches.at(it->second).get();
                    } else {
                        written.push_back(insertInto->name);
                    }
                } else {
                    written.push_back(insertInto->name);
                }
            }
            if (branchWrite != nullptr) {
                auto it = db._branchMapping.find(branchWrite->version);
                if (latching == Latching::Branch && it != db._branchMapping.end()) {
                    read.push_back(branchWrite->name);
                    writtenBranch = db._branches.at(it->second).get();
                    if (it->second == master_branch_id && _writesInPlace) {
                        _inPlaceTable = db.getTable(branchWrite->name);
                    }
                } else {
                    written.push_back(branchWrite->name);
                }
            }

            std::vector<std::pair<Table *, bool>> tables; // table, exclusive
            for (auto * names : {&read, &written}) {
                for (auto & name : *names) {
                    Table * table = db.getTable(name);
                    if (table != nullptr) tables.emplace_back(table, names == &written);
                }
            }
            // a table read and written is latched once, exclusive
            std::sort(tables.begin(), tables.end(), [](auto & a, auto & b) {
                return a.first < b.first || (a.first == b.first && a.second > b.second);
            });
            tables.erase(std::unique(tables.begin(), tables.end(), [](auto & a, auto & b) { return a.first == b.first; }), tables.end());
            for (auto & [table, exclusive] : tables) {
                if (exclusive) {
                    _exclusiveTables.emplace_back(table->_latch);
                } else {
                    _sharedTables.emplace_back(table->_latch);
                    if (latching == Latching::Branch && table != _inPlaceTable) _readTables.push_back(table);
                }
            }
            if (writtenBranch != nullptr) {
                _exclusiveBranch = std::unique_lock<SharedLatch>(writtenBranch->latch);
            }
            if (_inPlaceTable != nullptr) {
                std::lock_guard<std::mutex> lock(_inPlaceTable->_scanLatch);
                _inPlaceTable->_inPlaceWrites = &_inPlaceWrites;
            }
        }

        /// Latches the branches a read accesses, once the analysis has found them
        void latchReadBranches(Database & db, const std::set<branch_id_t> & branches) {
            if (_readsBranches) {
                for (branch_id_t branch : branches) {
                    auto it = db._branches.find(branch);
                    if (it != db._branches.end()) _sharedBranches.emplace_back(it->second->latch);
                }
            }
            // from now on, the statement waits for no other one
            for (Table * table : _readTables) {
                _epochs.emplace_back(table, table->_readers.enter());
            }
        }

        ~StatementLatches() {
            for (auto & [table, slot] : _epochs) {
                table->_readers.leave(slot);
            }
            if (_inPlaceTable != nullptr) {
                finish_in_place_writes(*_inPlaceTable, _inPlaceWrites);
            }
        }

    private:
        bool _readsBranches = false;
        bool _writesInPlace = false;
        std::vector<Table *> _readTables; // held shared, may be read in place
        std::vector<std::pair<Table *, unsigned>> _epochs; // the tables whose ReaderEpochs the statement has entered
        Table * _inPlaceTable = nullptr; // written in place by main beside readers of other branches
        InPlaceWrites _inPlaceWrites;
        // released in the reverse order: the branches, the tables, the insert latch, the catalog
        std::unique_lock<std::mutex> _serial;
        std::unique_lock<SharedLatch> _exclusiveCatalog;
        std::shared_lock<SharedLatch> _sharedCatalog;
        std::unique_lock<std::mutex> _insertLatch;
        std::vector<std::shared_lock<SharedLatch>> _sharedTables;
        std::vector<std::unique_lock<SharedLatch>> _exclusiveTables;
        std::unique_lock<SharedLatch> _exclusiveBranch;
        std::vector<std::shared_lock<SharedLatch>> _sharedBranches;
    };

    /// BEGIN, COMMIT and ROLLBACK create, merge and drop a branch: they hold the latches of a statement
    /// that changes the catalog
    class TransactionLatch {
    public:
        explicit TransactionLatch(Database & db) {
            if (latching == Latching::Serial) {
                _serial = std::unique_lock<std::mutex>(serialLatch);
            } else {
                _catalog = std::unique_lock<SharedLatch>(db._catalogLatch);
            }
        }

    private:
        std::unique_lock<std::mutex> _serial;
        std::unique_lock<SharedLatch> _catalog;
    };

    } // end anonymous namespace

    Transaction beginTransaction(Database & db, const std::string & branch) {
        static std::atomic<uint64_t> transactions(0);
        TransactionLatch latch(db);
        auto parent = db._branchMapping.find(branch);
        if (parent == db._branchMapping.end()) throw TransactionError("3D000", "branch \"" + branch + "\" does not exist");
        // identifiers do not begin with an underscore
        Transaction transaction{branch, "_tx" + std::to_string(++transactions)};
        branch_id_t snapshot = db.createBranch(transaction.snapshot, parent->second);
        db._branches.at(snapshot)->transaction = true;
        return transaction;
    }

    void commitTransaction(Database & db, const Transaction & transaction) {
        TransactionLatch latch(db);
        const branch_id_t snapshot = db._branchMapping.at(transaction.snapshot);
        MergeResult result = merge_branch(db, snapshot, db._branches.at(snapshot)->parent_id, MergePolicy::FirstCommitterWins);
        drop_branches(db, {snapshot});
        if (!result.conflicts.empty()) {
            const size_t shown = std::min<size_t>(result.conflicts.size(), 10);
            std::string message = "could not serialize access due to concurrent update; the transaction was rolled back (" +
                                  std::to_string(result.conflicts.size()) + " conflict" + (result.conflicts.size() > 1 ? "s" : "") + "):";
            for (size_t i = 0; i < shown; ++i) message += "\n  " + result.conflicts[i];
            if (shown < result.conflicts.size()) message += "\n  ...";
            throw TransactionError("40001", message);
        }
    }

    void rollbackTransaction(Database & db, const Transaction & transaction) {
        TransactionLatch latch(db);
        drop_branches(db, {db._branchMapping.at(transaction.snapshot)});
    }

    static llvm::Function *compileQuery(const std::string &query, std::unique_ptr<Operator> &queryTree, QueryContext &queryContext) {
        auto &codeGen = getThreadLocalCodeGen();
        auto &llvmContext = codeGen.getLLVMContext();
        auto &moduleGen = codeGen.getCurrentModuleGen();

        // query function signature
        llvm::FunctionType *funcTy = llvm::TypeBuilder<void(int, void *), false>::get(llvmContext);

        // the arrays the query computes live in arenas until the query ends
        struct ArenaResource : public ExecutionResource {
            Native::Sql::Arrays::QueryArenas arenas;
        };
        auto arenas = std::make_unique<ArenaResource>();
        Native::Sql::Arrays::ArenaScope arenaScope(arenas->arenas);
        queryContext.executionContext.acquireResource(std::move(arenas));

        llvm::Function *queryFunc;
        {
            FunctionGen funcGen(moduleGen, "query", funcTy);

            auto physicalTree = Algebra::translateToPhysicalTree(*queryTree,queryContext);
            physicalTree->produce();

            queryFunc = funcGen.getFunction();
        }

        return queryFunc;
    }

    void printFunction(Native::Sql::SqlTuple *tuple) {
        std::cout << Native::Sql::toString(*tuple) << "\n";
    }

    static void compileAndExecuteStatement(const std::string &query, Database &db, void *callbackFunction);

    void compileAndExecute(const std::string &query, Database &db, void *callbackFunction) {
        try {
            compileAndExecuteStatement(query, db, callbackFunction);
        } catch (const semanticalAnalysis::general_aggregation_required &) {
            auto print = (callbackFunction != nullptr) ? reinterpret_cast<void (*)(Native::Sql::SqlTuple *)>(callbackFunction) : &printFunction;
            execute(query, db, "master", [print](Native::Sql::SqlTuple * tuple) { print(tuple); });
        }
    }

    static void compileAndExecuteStatement(const std::string &query, Database &db, void *callbackFunction) {
        {
            // WITH, UNION, VALUES and SELECT without FROM run as the statements of a session
            std::string lower;
            std::transform(query.begin(), query.end(), std::back_inserter(lower), ::tolower);
            size_t first = lower.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && (lower.compare(first, 4, "with") == 0 || lower.compare(first, 6, "values") == 0 ||
                lower.compare(first, 1, "(") == 0 || (lower.compare(first, 6, "select") == 0 &&
                (lower.find("union") != std::string::npos || lower.find("intersect") != std::string::npos ||
                 lower.find("except") != std::string::npos || lower.find(" from ") == std::string::npos)))) {
                auto print = (callbackFunction != nullptr) ? reinterpret_cast<void (*)(Native::Sql::SqlTuple *)>(callbackFunction) : &printFunction;
                execute(query, db, "master", [print](Native::Sql::SqlTuple * tuple) { print(tuple); });
                return;
            }
        }
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

#if USE_HYRISE
        hsql::SQLParser::parse(query, &queryContext.hyriseResult);
        if (!queryContext.hyriseResult.isValid()) throw std::runtime_error("invalid statement");
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.hyriseResult);
#else
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.parsingContext);
#endif
        StatementLatches latches(db, queryContext.analyzingContext.parserResult);

        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        analyser->verify();
        analyser->constructTree();
        auto &queryTree = queryContext.analyzingContext.joinedTree;
        if (queryTree == nullptr) return;
        latches.latchReadBranches(db, queryContext.analyzingContext.branchIds);
        if (callbackFunction == nullptr) callbackFunction = (queryContext.analyzingContext.callback != nullptr) ? queryContext.analyzingContext.callback : (void*)&printFunction;

        auto queryFunc = compileQuery(query, queryTree,queryContext);
        if (queryFunc == nullptr) return;

        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds,queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::executeFunction(queryFunc, args, callbackFunction);
    }

    static const char * statementTag(semanticalAnalysis::SQLParserResult::OpType type) {
        using Op = semanticalAnalysis::SQLParserResult::OpType;
        switch (type) {
            case Op::Select: return "SELECT";
            case Op::Insert: return "INSERT";
            case Op::Update: return "UPDATE";
            case Op::Delete: return "DELETE";
            case Op::CreateTable: return "CREATE TABLE";
            case Op::CreateBranch: return "CREATE BRANCH";
            case Op::Copy: return "COPY";
            case Op::CreateIndex: return "CREATE INDEX";
            case Op::DropBranch: return "DROP BRANCH";
            case Op::MergeBranch: return "MERGE";
            case Op::AlterTable: return "ALTER TABLE";
            default: return "";
        }
    }

    /// Describes the columns of a query's result, named as in the select list (aggregates by their function)
    static void describeResult(Algebra::Logical::Result & root, const semanticalAnalysis::SQLParserResult & parserResult, StatementResult & result) {
        result.returnsRows = true;
        const semanticalAnalysis::SelectStatement * select =
                (parserResult.opType == semanticalAnalysis::SQLParserResult::OpType::Select) ? parserResult.selectStmt : nullptr;
        bool aligned = select != nullptr && std::none_of(select->projections.begin(), select->projections.end(),
                [](const semanticalAnalysis::Column & c) { return c.name == "*" && c.aggregate.empty(); });
        size_t position = 0;
        for (iu_p_t iu : root.selection) {
            if (iu->iuType == InformationUnit::Type::ColumnRef && getColumnInformation(iu)->columnName == "tid") {
                continue; // TupleStream leaves it out
            }
            std::string name = "?column?";
            if (aligned && position < select->projections.size()) {
                auto & projection = select->projections[position];
                name = !projection.alias.empty() ? projection.alias : projection.aggregate.empty() ? projection.name : projection.aggregate;
            } else if (iu->iuType == InformationUnit::Type::ColumnRef) {
                name = getColumnInformation(iu)->columnName;
            }
            result.columnNames.push_back(name);
            result.columnTypes.push_back(iu->sqlType);
            ++position;
        }
    }

    /// \returns Whether the statement reads the row of a SELECT without FROM
    static bool readsOneRow(const semanticalAnalysis::SQLParserResult & statement) {
        if (statement.opType != semanticalAnalysis::SQLParserResult::OpType::Select) return false;
        auto & relations = statement.selectStmt->relations;
        return std::any_of(relations.begin(), relations.end(), [](const semanticalAnalysis::Relation & r) {
            return r.name == sqlExpressions::oneRowTable;
        });
    }

    /// Provides the row of a SELECT without FROM, a temporary table, once per statement
    static void provideOneRowTable(Database & db, TemporaryTables & temporaries, const semanticalAnalysis::SQLParserResult & statement) {
        if (!readsOneRow(statement) || TemporaryTables::find(db, sqlExpressions::oneRowTable) != nullptr) return;
        Table & table = temporaries.create({{sqlExpressions::oneRowTable, Sql::getIntegerTy(false)}});
        QueryContext context(db);
        context.executionContext.branchId = master_branch_id;
        std::vector<Native::Sql::value_op_t> values;
        values.push_back(Native::Sql::Integer::castString("0"));
        Native::Sql::SqlTuple tuple(std::move(values));
        insert_tuple(tuple, table, context);
        temporaries.bind(sqlExpressions::oneRowTable, &table);
    }

    namespace with {

    /// SELECTs combined by UNION, INTERSECT and EXCEPT [ ALL ]: a SELECT, or an operation on two queries
    struct SetQuery {
        std::string term;                 // the text of a SELECT, if there is no operation
        std::string op;                   // "union", "intersect" or "except"
        bool all = false;                 // ALL: duplicates count, as in a multiset
        std::unique_ptr<SetQuery> left, right;
        std::set<std::string> relations;  // the relations the SELECT names
        bool isTerm() const { return op.empty(); }
    };
    using set_query_p = std::unique_ptr<SetQuery>;

    struct CommonTableExpression {
        std::string name;
        std::vector<std::string> columns; // empty: the names of the first query's columns
        set_query_p body;
    };

    /// [ WITH [ RECURSIVE ] name [ ( columns ) ] AS ( query ) [ , ... ] ] query
    struct WithStatement {
        bool recursive = false;
        std::vector<CommonTableExpression> ctes;
        set_query_p query;
    };

    struct Lexeme {
        enum Kind { Word, Quoted, String, Open, Close, Comma, Semicolon, Other } kind;
        size_t begin, end; // in the statement
        std::string word;  // a Word in lower case, the text of a Quoted identifier or a String
    };

    static tardisParser::syntactical_error syntaxError(const std::string & message) {
        return tardisParser::syntactical_error(message);
    }

    /// The words, quoted identifiers, strings and parentheses of a statement, without comments
    static std::vector<Lexeme> lex(const std::string & sql) {
        std::vector<Lexeme> lexemes;
        const size_t n = sql.size();
        size_t i = 0;
        while (i < n) {
            const char c = sql[i];
            if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
            if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
                while (i < n && sql[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
                size_t e = sql.find("*/", i + 2);
                i = (e == std::string::npos) ? n : e + 2;
                continue;
            }
            Lexeme lexeme{Lexeme::Other, i, i + 1, ""};
            if (c == '\'' || c == '"') {
                size_t j = i + 1;
                while (true) {
                    if (j >= n) throw syntaxError(c == '\'' ? "unterminated string" : "unterminated quoted identifier");
                    if (sql[j] == c) {
                        if (j + 1 < n && sql[j + 1] == c) { lexeme.word += c; j += 2; continue; }
                        break;
                    }
                    lexeme.word += sql[j++];
                }
                lexeme.kind = (c == '\'') ? Lexeme::String : Lexeme::Quoted;
                lexeme.end = j + 1;
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i;
                while (j < n && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_' || sql[j] == '$')) ++j;
                lexeme.kind = Lexeme::Word;
                lexeme.end = j;
                std::transform(sql.begin() + i, sql.begin() + j, std::back_inserter(lexeme.word), ::tolower);
            } else if (c == '(') {
                lexeme.kind = Lexeme::Open;
            } else if (c == ')') {
                lexeme.kind = Lexeme::Close;
            } else if (c == ',') {
                lexeme.kind = Lexeme::Comma;
            } else if (c == ';') {
                lexeme.kind = Lexeme::Semicolon;
            }
            i = lexeme.end;
            lexemes.push_back(std::move(lexeme));
        }
        return lexemes;
    }

    static bool isWord(const std::vector<Lexeme> & lexemes, size_t i, const char * word) {
        return i < lexemes.size() && lexemes[i].kind == Lexeme::Word && lexemes[i].word == word;
    }

    /// \returns The closing parenthesis of the one at \p open
    static size_t matching(const std::vector<Lexeme> & lexemes, size_t open, size_t to) {
        size_t depth = 0;
        for (size_t i = open; i < to; ++i) {
            if (lexemes[i].kind == Lexeme::Open) ++depth;
            else if (lexemes[i].kind == Lexeme::Close && --depth == 0) return i;
        }
        throw syntaxError("unbalanced parentheses");
    }

    /// \returns The texts of the items of [from, to) separated by commas outside parentheses
    static std::vector<std::string> splitItems(const std::string & sql, const std::vector<Lexeme> & lexemes, size_t from, size_t to) {
        std::vector<std::string> items;
        size_t depth = 0, start = from;
        for (size_t i = from; i <= to; ++i) {
            if (i == to || (depth == 0 && lexemes[i].kind == Lexeme::Comma)) {
                if (start >= i) throw syntaxError("an item of a list is missing");
                items.push_back(sql.substr(lexemes[start].begin, lexemes[i - 1].end - lexemes[start].begin));
                start = i + 1;
            } else if (lexemes[i].kind == Lexeme::Open) {
                ++depth;
            } else if (lexemes[i].kind == Lexeme::Close) {
                --depth;
            }
        }
        return items;
    }

    static bool isSetOperator(const Lexeme & lexeme) {
        return lexeme.kind == Lexeme::Word && (lexeme.word == "union" || lexeme.word == "intersect" || lexeme.word == "except");
    }

    static set_query_p makeTerm(std::string text) {
        auto query = std::make_unique<SetQuery>();
        query->term = std::move(text);
        return query;
    }

    static set_query_p combine(const std::string & op, bool all, set_query_p left, set_query_p right) {
        auto query = std::make_unique<SetQuery>();
        query->op = op;
        query->all = all;
        query->left = std::move(left);
        query->right = std::move(right);
        return query;
    }

    /// The query of [from, to): INTERSECT binds closer than UNION and EXCEPT, which combine from left to
    /// right, as in PostgreSQL; a query in parentheses is an operand; VALUES ( ... ) [ , ... ] is a SELECT
    /// per row (with the columns column1, column2, ...) combined by UNION ALL
    static set_query_p parseSetQuery(const std::string & sql, const std::vector<Lexeme> & lexemes, size_t from, size_t to) {
        struct Operator { std::string op; bool all; };
        std::vector<std::pair<size_t, size_t>> operands;
        std::vector<Operator> operators;
        size_t depth = 0, start = from;
        for (size_t i = from; i < to; ++i) {
            const Lexeme & lexeme = lexemes[i];
            if (lexeme.kind == Lexeme::Open) {
                ++depth;
            } else if (lexeme.kind == Lexeme::Close) {
                if (depth == 0) throw syntaxError("unbalanced parentheses");
                --depth;
            } else if (depth == 0 && isSetOperator(lexeme)) {
                operands.emplace_back(start, i);
                start = i + 1;
                const bool all = isWord(lexemes, start, "all");
                if (all || isWord(lexemes, start, "distinct")) ++start;
                operators.push_back({lexeme.word, all});
                i = start - 1;
            }
        }
        if (depth != 0) throw syntaxError("unbalanced parentheses");
        operands.emplace_back(start, to);

        auto operand = [&](size_t a, size_t b) -> set_query_p {
            if (a >= b) throw syntaxError("a query of a set operation is missing");
            if (lexemes[a].kind == Lexeme::Open && matching(lexemes, a, b) == b - 1) return parseSetQuery(sql, lexemes, a + 1, b - 1);
            if (!isWord(lexemes, a, "values")) return makeTerm(sql.substr(lexemes[a].begin, lexemes[b - 1].end - lexemes[a].begin));
            set_query_p rows;
            for (size_t i = a + 1; i < b; ) {
                if (lexemes[i].kind != Lexeme::Open) throw syntaxError("Expected '(' of a row of VALUES");
                size_t close = matching(lexemes, i, b);
                auto values = splitItems(sql, lexemes, i + 1, close);
                std::string term = "SELECT ";
                for (size_t v = 0; v < values.size(); ++v) {
                    term += (v > 0 ? ", " : "") + values[v] + " AS column" + std::to_string(v + 1);
                }
                rows = rows ? combine("union", true, std::move(rows), makeTerm(term)) : makeTerm(term);
                i = close + 1;
                if (i < b) {
                    if (lexemes[i].kind != Lexeme::Comma) throw syntaxError("Expected ',' between the rows of VALUES");
                    ++i;
                }
            }
            if (!rows) throw syntaxError("VALUES without rows");
            return rows;
        };
        // INTERSECT first, then UNION and EXCEPT from left to right
        std::vector<set_query_p> queries;
        std::vector<Operator> rest;
        queries.push_back(operand(operands[0].first, operands[0].second));
        for (size_t k = 0; k < operators.size(); ++k) {
            auto next = operand(operands[k + 1].first, operands[k + 1].second);
            if (operators[k].op == "intersect") {
                queries.back() = combine("intersect", operators[k].all, std::move(queries.back()), std::move(next));
            } else {
                rest.push_back(operators[k]);
                queries.push_back(std::move(next));
            }
        }
        set_query_p query = std::move(queries[0]);
        for (size_t k = 0; k < rest.size(); ++k) query = combine(rest[k].op, rest[k].all, std::move(query), std::move(queries[k + 1]));
        return query;
    }

    /// \returns The name of a common table expression or of one of its columns
    static std::string nameAt(const std::string & sql, const std::vector<Lexeme> & lexemes, size_t i, const char * what) {
        if (i >= lexemes.size() || (lexemes[i].kind != Lexeme::Word && lexemes[i].kind != Lexeme::Quoted)) {
            throw syntaxError(std::string("Expected the name of ") + what);
        }
        if (lexemes[i].kind == Lexeme::Quoted) return lexemes[i].word;
        return sql.substr(lexemes[i].begin, lexemes[i].end - lexemes[i].begin); // as written, like the names of tables
    }

    /// \returns Whether execute() runs the statement here: WITH, queries combined by UNION, INTERSECT or
    /// EXCEPT, or VALUES
    static bool parseSetStatement(const std::string & sql, WithStatement & statement) {
        // most statements need no lexing: those that start with neither SELECT, WITH, VALUES nor '('
        size_t first = sql.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return false;
        std::string head;
        for (size_t i = first; i < sql.size() && head.size() < 6 && std::isalpha(static_cast<unsigned char>(sql[i])); ++i) {
            head += static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i])));
        }
        const bool select = head == "select" || sql[first] == '(';
        if (!select && head.rfind("with", 0) != 0 && head != "values") return false;
        if (select) {
            std::string lower;
            std::transform(sql.begin(), sql.end(), std::back_inserter(lower), ::tolower);
            if (lower.find("union") == std::string::npos && lower.find("intersect") == std::string::npos &&
                lower.find("except") == std::string::npos) return false;
        }

        auto lexemes = lex(sql);
        while (!lexemes.empty() && lexemes.back().kind == Lexeme::Semicolon) lexemes.pop_back();
        const size_t n = lexemes.size();
        if (n == 0) return false;
        if (!isWord(lexemes, 0, "with")) {
            bool setQuery = isWord(lexemes, 0, "values");
            size_t depth = 0;
            for (auto & lexeme : lexemes) {
                if (lexeme.kind == Lexeme::Open) ++depth;
                else if (lexeme.kind == Lexeme::Close && depth > 0) --depth;
                else if (depth == 0 && isSetOperator(lexeme)) setQuery = true;
            }
            if (!setQuery) return false;
            statement.query = parseSetQuery(sql, lexemes, 0, n);
            return true;
        }

        size_t i = 1;
        if (isWord(lexemes, i, "recursive")) {
            statement.recursive = true;
            ++i;
        }
        while (true) {
            CommonTableExpression cte;
            cte.name = nameAt(sql, lexemes, i++, "a common table expression");
            if (i < n && lexemes[i].kind == Lexeme::Open) {
                size_t close = matching(lexemes, i, n);
                for (size_t c = i + 1; c < close; c += 2) {
                    cte.columns.push_back(nameAt(sql, lexemes, c, "a column"));
                    if (c + 1 < close && lexemes[c + 1].kind != Lexeme::Comma) throw syntaxError("Expected ',' between the columns");
                }
                if (cte.columns.empty()) throw syntaxError("Expected the columns of '" + cte.name + "'");
                i = close + 1;
            }
            if (!isWord(lexemes, i, "as")) throw syntaxError("Expected AS after the common table expression '" + cte.name + "'");
            ++i;
            if (isWord(lexemes, i, "not") && isWord(lexemes, i + 1, "materialized")) i += 2;
            else if (isWord(lexemes, i, "materialized")) ++i;
            if (i >= n || lexemes[i].kind != Lexeme::Open) throw syntaxError("Expected '(' with the query of '" + cte.name + "'");
            size_t close = matching(lexemes, i, n);
            cte.body = parseSetQuery(sql, lexemes, i + 1, close);
            i = close + 1;
            statement.ctes.push_back(std::move(cte));
            if (i < n && lexemes[i].kind == Lexeme::Comma) {
                ++i;
                continue;
            }
            break;
        }
        if (i >= n) throw syntaxError("Expected a query after WITH");
        statement.query = parseSetQuery(sql, lexemes, i, n);
        return true;
    }

    /// A row of a set query, in the types of its columns, and the text that tells duplicates apart
    struct Row {
        std::string key;
        std::vector<Native::Sql::value_op_t> values;
    };

    /// \returns The rows of \p left op \p right: without ALL, each row once; with ALL, UNION adds the
    /// counts of a row, INTERSECT takes the lesser and EXCEPT subtracts them, as in PostgreSQL
    static std::vector<Row> combineRows(const std::string & op, bool all, std::vector<Row> left, std::vector<Row> right) {
        std::vector<Row> rows;
        std::unordered_set<std::string> emitted;
        auto emit = [&](Row & row) {
            if (all || emitted.insert(row.key).second) rows.push_back(std::move(row));
        };
        if (op == "union") {
            for (auto & row : left) emit(row);
            for (auto & row : right) emit(row);
            return rows;
        }
        std::unordered_map<std::string, size_t> counts; // of the right rows that no left row took yet
        for (auto & row : right) ++counts[row.key];
        for (auto & row : left) {
            auto it = counts.find(row.key);
            const bool inRight = it != counts.end() && it->second > 0;
            if (inRight && all) --it->second;
            if (inRight == (op == "intersect")) emit(row);
        }
        return rows;
    }

    /// The type of a column of a common table expression or of a set operation: nullable, and text for all strings,
    /// since the queries may compute longer strings than the first one
    static Sql::SqlType columnType(Sql::SqlType type) {
        using ID = Sql::SqlType::TypeID;
        if (type.typeID == ID::CharID || type.typeID == ID::VarcharID) return Sql::getTextTy(true);
        if (type.typeID == ID::NumericID) return Sql::getNumericFullLengthTy(type.numericLayout.precision, true);
        return Sql::toNullableTy(type);
    }

    /// \returns The type of a column whose queries give \p a and \p b (see columnType()): numbers take the
    /// greater of integer < bigint < numeric (of the greater scale) < double precision, as in PostgreSQL
    static Sql::SqlType unifyTypes(Sql::SqlType a, Sql::SqlType b, size_t column) {
        using ID = Sql::SqlType::TypeID;
        a = columnType(a);
        b = columnType(b);
        if (a.typeID == ID::UnknownID) return b; // NULL
        if (b.typeID == ID::UnknownID) return a;
        if (Sql::equals(a, b, Sql::SqlTypeEqualsMode::Full)) return a;
        auto rank = [](ID id) {
            return id == ID::IntegerID ? 1 : id == ID::LongIntegerID ? 2 : id == ID::NumericID ? 3 : id == ID::DoubleID ? 4 : 0;
        };
        const int ra = rank(a.typeID), rb = rank(b.typeID);
        if (ra > 0 && rb > 0) {
            if (std::max(ra, rb) == 4) return Sql::getDoubleTy(true);
            if (std::max(ra, rb) == 3) {
                uint8_t scale = std::max(ra == 3 ? a.numericLayout.precision : 0, rb == 3 ? b.numericLayout.precision : 0);
                return Sql::getNumericFullLengthTy(scale, true);
            }
            return Sql::getLongIntegerTy(true);
        }
        throw semanticalAnalysis::semantic_sql_error("the types " + Sql::getName(a) + " and " + Sql::getName(b) +
                                                     " of column " + std::to_string(column + 1) + " cannot be matched");
    }

    /// \returns The values of a result tuple in \p types (they own their strings, while the tuple's values
    /// may point into memory of its query), and in \p key the text that tells duplicates apart
    static std::vector<Native::Sql::value_op_t> convertRow(const Native::Sql::SqlTuple & tuple, const std::vector<Sql::SqlType> & types,
                                                           std::string & key) {
        if (tuple.values.size() != types.size()) {
            throw semanticalAnalysis::semantic_sql_error("each query of a set operation has the same number of columns");
        }
        std::vector<Native::Sql::value_op_t> values;
        key.clear();
        for (size_t i = 0; i < types.size(); ++i) {
            const Native::Sql::Value & value = *tuple.values[i];
            if (Native::Sql::isNullable(value) && static_cast<const Native::Sql::NullableValue &>(value).isNull()) {
                values.push_back(Native::Sql::castColumnValue(Native::Sql::nullLiteral, types[i]));
                key += 'N';
                continue;
            }
            const std::string text = Native::Sql::toString(Native::Sql::getAssociatedValue(value));
            try {
                values.push_back(Native::Sql::castColumnValue(text, types[i]));
            } catch (...) {
                throw semanticalAnalysis::semantic_sql_error("'" + text + "' of column " + std::to_string(i + 1) +
                        " is no value of the column's type " + Sql::getName(types[i]));
            }
            // duplicates as values of the column: 1 and 1.0 of a numeric column are one
            const std::string converted = Native::Sql::toString(Native::Sql::getAssociatedValue(*values.back()));
            key += 'V';
            key += std::to_string(converted.size());
            key += ':';
            key += converted;
        }
        return values;
    }

    } // end namespace with

    static StatementResult runSelect(QueryContext & queryContext, const std::string & text, Database & db, const std::string & branch,
                                     const Transaction * transaction, TemporaryTables & temporaries,
                                     const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                     const std::function<void(const StatementResult &)> & described, bool describeOnly);

    namespace aggregation {

    using sqlExpressions::Node;
    using semanticalAnalysis::Column;
    using semanticalAnalysis::semantic_sql_error;

    bool isAggregateName(const std::string & name) {
        return name == "count" || name == "sum" || name == "avg" || name == "min" || name == "max";
    }

    struct Aggregate {
        std::string function;
        bool star = false; // count(*)
        Node argument;
    };

    /// An aggregate query as two queries: the first computes the GROUP BY columns and the arguments of the
    /// aggregates (_g0, ..., _a0, ...) of the rows; the groups and their aggregates go to a temporary table,
    /// over which the select list (outputs) runs
    struct Plan {
        std::vector<Column> groups;
        std::vector<Aggregate> aggregates;
        std::vector<Column> outputs;

        static Node columnNode(const std::string & name, const std::string & table = std::string()) {
            Node n;
            n.kind = Node::Kind::Column;
            n.name = name;
            n.table = table;
            return n;
        }

        size_t group(const std::string & table, const std::string & name) const {
            for (size_t j = 0; j < groups.size(); ++j) {
                if (groups[j].name == name && (groups[j].table == table || groups[j].table.empty() || table.empty())) return j;
            }
            throw semantic_sql_error("column \"" + (table.empty() ? name : table + "." + name) +
                                     "\" must appear in the GROUP BY clause or be used in an aggregate function");
        }

        size_t add(const std::string & function, bool star, Node argument) {
            if (!star && semanticalAnalysis::containsAggregate(argument)) throw semantic_sql_error("aggregate function calls cannot be nested");
            aggregates.push_back({function, star, std::move(argument)});
            return aggregates.size() - 1;
        }

        /// \returns The expression over the table of the groups
        Node rewrite(const Node & n) {
            if (n.kind == Node::Kind::Function && isAggregateName(n.name)) {
                const bool star = n.name == "count" && n.value == "*" && n.children.empty();
                if (!star && n.children.size() != 1) throw semantic_sql_error("the aggregate " + n.name + " takes one argument");
                return columnNode("_a" + std::to_string(add(n.name, star, star ? Node() : n.children[0])));
            }
            if (n.kind == Node::Kind::Column) return columnNode("_g" + std::to_string(group(n.table, n.name)));
            Node copy = n;
            for (auto & child : copy.children) child = rewrite(child);
            return copy;
        }

        explicit Plan(const semanticalAnalysis::SelectStatement & select) : groups(select.groupBy) {
            if (select.projections.empty()) throw semantic_sql_error("SELECT * cannot be used with aggregates or GROUP BY");
            for (auto & projection : select.projections) {
                Column out;
                out.alias = !projection.alias.empty() ? projection.alias : !projection.aggregate.empty() ? projection.aggregate : projection.name;
                if (!projection.aggregate.empty()) {
                    const bool star = projection.name == "*";
                    out.name = "_a" + std::to_string(add(projection.aggregate, star, star ? Node() : columnNode(projection.name, projection.table)));
                } else if (projection.expression) {
                    out.name = projection.name;
                    out.expression = std::make_shared<Node>(rewrite(*projection.expression));
                } else if (projection.name == "*") {
                    throw semantic_sql_error("SELECT * cannot be used with aggregates or GROUP BY");
                } else {
                    out.name = "_g" + std::to_string(group(projection.table, projection.name));
                }
                outputs.push_back(std::move(out));
            }
        }
    };

    using TypeID = Sql::SqlType::TypeID;

    bool isNumber(Sql::SqlType t) {
        return t.typeID == TypeID::IntegerID || t.typeID == TypeID::LongIntegerID || t.typeID == TypeID::NumericID || t.typeID == TypeID::DoubleID;
    }

    /// The type of an aggregate of an input type, as BranchAggregation types those of numbers: counts and sums
    /// of integers, and all aggregates of bigints, are numerics of 18 digits without a fraction; the others
    /// keep the type of their input. sum and avg of arrays of numbers are arrays of double precision.
    Sql::SqlType resultType(const Aggregate & aggregate, Sql::SqlType input) {
        if (aggregate.function == "count") return Sql::getNumericFullLengthTy(0, false);
        const bool arithmetic = aggregate.function == "sum" || aggregate.function == "avg";
        // a varchar of castColumnValue points into its text, which is gone when the table of the groups is read
        if (input.typeID == TypeID::CharID || input.typeID == TypeID::VarcharID) {
            if (arithmetic) throw semantic_sql_error(aggregate.function + " takes numbers or arrays of numbers, not " + Sql::getName(Sql::toNotNullableTy(input)));
            return Sql::getTextTy(true);
        }
        if (input.typeID == TypeID::ArrayID) {
            if (!arithmetic) return Sql::toNullableTy(input);
            if (!isNumber(Sql::getElementTy(input))) {
                throw semantic_sql_error(aggregate.function + " takes numbers or arrays of numbers, not " + Sql::getName(Sql::toNotNullableTy(input)));
            }
            return Sql::getArrayTy(Sql::getDoubleTy(), true);
        }
        if (arithmetic && !isNumber(input)) {
            throw semantic_sql_error(aggregate.function + " takes numbers or arrays of numbers, not " + Sql::getName(Sql::toNotNullableTy(input)));
        }
        if (input.typeID == TypeID::LongIntegerID || (aggregate.function == "sum" && input.typeID == TypeID::IntegerID)) {
            return Sql::getNumericFullLengthTy(0, true);
        }
        if (input.typeID == TypeID::UnknownID) return Sql::getIntegerTy(true);
        return Sql::toNullableTy(input);
    }

    /// The state of an aggregate of a group
    struct Accumulator {
        uint64_t count = 0;            // of the values that are not NULL
        __int128 integers = 0;         // the sum of integers, bigints and numerics (scaled)
        double doubles = 0;            // the sum of doubles
        Native::Sql::Arrays::NumberArray array; // the sums of arrays, element by element
        bool hasBest = false;          // of min and max: the text of the least or greatest value (a varchar of
        std::string best;              // castColumnValue points into its text, so the text is kept, not the value)
    };

    template<typename T>
    int order(T a, T b) { return a < b ? -1 : (b < a ? 1 : 0); }

    /// \returns The order of two values of a type, as PostgreSQL's btree orders them (min, max)
    int compareValues(const Native::Sql::Value & a, const Native::Sql::Value & b, Sql::SqlType type) {
        using namespace Native::Sql;
        switch (type.typeID) {
            case TypeID::IntegerID: return order(static_cast<const Integer &>(a).value, static_cast<const Integer &>(b).value);
            case TypeID::LongIntegerID:
            case TypeID::NumericID: return order(static_cast<const Numeric &>(a).value, static_cast<const Numeric &>(b).value);
            case TypeID::DoubleID: {
                const double x = static_cast<const Double &>(a).value, y = static_cast<const Double &>(b).value;
                if (std::isnan(x) || std::isnan(y)) return order(std::isnan(x), std::isnan(y)); // NaN is the greatest
                return order(x, y);
            }
            case TypeID::BoolID: return order(static_cast<const Bool &>(a).value, static_cast<const Bool &>(b).value);
            case TypeID::DateID: return order(static_cast<const Date &>(a).value, static_cast<const Date &>(b).value);
            case TypeID::TimestampID: return order(static_cast<const Timestamp &>(a).value, static_cast<const Timestamp &>(b).value);
            case TypeID::TextID: return order(static_cast<const Text &>(a).getView(), static_cast<const Text &>(b).getView());
            case TypeID::CharID:
            case TypeID::VarcharID: {
                auto view = [](const Value & v) {
                    if (auto * text = dynamic_cast<const Text *>(&v)) return text->getView();
                    auto & varchar = static_cast<const Varchar &>(v);
                    return std::string_view(reinterpret_cast<const char *>(varchar.begin()), varchar.length());
                };
                return order(view(a), view(b));
            }
            case TypeID::ArrayID:
                return Arrays::compare(static_cast<const Array &>(a).value, static_cast<const Array &>(b).value, Sql::toNotNullableTy(type));
            default:
                throw semantic_sql_error("min and max do not take " + Sql::getName(Sql::toNotNullableTy(type)));
        }
    }

    void accumulate(const Aggregate & aggregate, Sql::SqlType input, Accumulator & acc, const Native::Sql::Value * value) {
        using namespace Native::Sql;
        if (aggregate.star) { ++acc.count; return; }
        if (value == nullptr) return; // NULL
        ++acc.count;
        if (aggregate.function == "count") return;
        const Value & v = getAssociatedValue(*value);
        if (aggregate.function == "min" || aggregate.function == "max") {
            int c = 0;
            if (acc.hasBest) {
                const value_op_t best = castColumnValue(acc.best, Sql::toNotNullableTy(input));
                c = compareValues(v, *best, input);
            }
            if (!acc.hasBest || (aggregate.function == "min" ? c < 0 : c > 0)) {
                acc.best = toString(v);
                acc.hasBest = true;
            }
            return;
        }
        switch (input.typeID) {
            case TypeID::IntegerID: acc.integers += static_cast<const Integer &>(v).value; return;
            case TypeID::LongIntegerID:
            case TypeID::NumericID: acc.integers += static_cast<const Numeric &>(v).value; return;
            case TypeID::DoubleID: acc.doubles += static_cast<const Double &>(v).value; return;
            case TypeID::ArrayID: {
                auto numbers = Arrays::toNumbers(static_cast<const Array &>(v).value, Sql::getElementTy(input));
                if (acc.count == 1) { acc.array = std::move(numbers); return; }
                if (numbers.widths != acc.array.widths) throw semantic_sql_error(aggregate.function + " of arrays of different shapes");
                for (size_t i = 0; i < numbers.values.size(); ++i) {
                    acc.array.values[i] += numbers.values[i];
                    if (numbers.nulls[i]) acc.array.nulls[i] = true;
                }
                return;
            }
            default: return;
        }
    }

    std::string integerText(__int128 value) {
        if (value > INT64_MAX || value < INT64_MIN) throw semantic_sql_error("numeric field overflow in an aggregate");
        return std::to_string(static_cast<int64_t>(value));
    }

    /// \returns The text of the aggregate (for castColumnValue), or nullLiteral
    std::string finish(const Aggregate & aggregate, Sql::SqlType input, Sql::SqlType result, const Accumulator & acc) {
        using namespace Native::Sql;
        if (aggregate.function == "count") return std::to_string(acc.count);
        if (acc.count == 0) return nullLiteral;
        if (aggregate.function == "min" || aggregate.function == "max") return acc.best;
        const bool avg = aggregate.function == "avg";
        switch (input.typeID) {
            case TypeID::IntegerID:
            case TypeID::LongIntegerID:
                return integerText(avg ? acc.integers / static_cast<__int128>(acc.count) : acc.integers);
            case TypeID::NumericID: {
                const __int128 raw = avg ? acc.integers / static_cast<__int128>(acc.count) : acc.integers;
                integerText(raw);
                return formatNumeric(static_cast<int64_t>(raw), result.numericLayout.precision);
            }
            case TypeID::DoubleID: return formatDouble(avg ? acc.doubles / static_cast<double>(acc.count) : acc.doubles);
            case TypeID::ArrayID: {
                Arrays::NumberArray numbers = acc.array;
                if (avg) for (double & x : numbers.values) x /= static_cast<double>(acc.count);
                return Arrays::format(Arrays::fromNumbers(numbers, nullptr), Sql::toNotNullableTy(result));
            }
            default: return nullLiteral;
        }
    }

    } // end namespace aggregation

    /// An aggregate query that BranchAggregation does not compute (aggregates of expressions, in
    /// expressions, over several tables or temporary tables, of arrays or text, GROUP BY columns): the GROUP
    /// BY columns and the arguments of the aggregates of its rows (a query), their groups aggregated here
    /// into a temporary table, and the select list over it (another query)
    static StatementResult runAggregation(const semanticalAnalysis::SelectStatement & select, Database & db, const std::string & branch,
                                          const Transaction * transaction, TemporaryTables & temporaries,
                                          const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                          const std::function<void(const StatementResult &)> & described, bool describeOnly) {
        using namespace aggregation;
        using Native::Sql::nullLiteral;
        Plan plan(select);

        // the rows: GROUP BY columns and arguments
        auto rows = std::make_unique<semanticalAnalysis::SelectStatement>();
        rows->relations = select.relations;
        rows->joinConditions = select.joinConditions;
        rows->selections = select.selections;
        rows->predicates = select.predicates;
        for (size_t j = 0; j < plan.groups.size(); ++j) {
            Column c = plan.groups[j];
            c.alias = "_g" + std::to_string(j);
            rows->projections.push_back(std::move(c));
        }
        std::vector<int> argumentColumn(plan.aggregates.size(), -1);
        for (size_t k = 0; k < plan.aggregates.size(); ++k) {
            const Aggregate & aggregate = plan.aggregates[k];
            if (aggregate.star) continue;
            Column c;
            if (aggregate.argument.kind == Node::Kind::Column) {
                c.name = aggregate.argument.name;
                c.table = aggregate.argument.table;
            } else {
                c.name = "?column?";
                c.expression = std::make_shared<Node>(aggregate.argument);
            }
            c.alias = "_a" + std::to_string(k);
            argumentColumn[k] = static_cast<int>(rows->projections.size());
            rows->projections.push_back(std::move(c));
        }
        if (rows->projections.empty()) {
            // count(*) alone: a constant per row
            Column c;
            c.name = "?column?";
            c.alias = "_one";
            Node one;
            one.kind = Node::Kind::Literal;
            one.value = "1";
            c.expression = std::make_shared<Node>(one);
            rows->projections.push_back(std::move(c));
        }

        std::vector<Sql::SqlType> rowTypes;
        struct Group {
            std::vector<std::string> values; // of the GROUP BY columns (text, or nullLiteral)
            std::vector<Accumulator> accumulators;
        };
        std::unordered_map<std::string, size_t> index;
        std::vector<Group> groups;
        {
            QueryContext context(db);
            context.analyzingContext.parserResult.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
            context.analyzingContext.parserResult.selectStmt = rows.release();
            runSelect(context, "aggregate rows", db, branch, transaction, temporaries, [&](Native::Sql::SqlTuple * tuple) {
                std::string key;
                std::vector<std::string> values;
                for (size_t j = 0; j < plan.groups.size(); ++j) {
                    const Native::Sql::Value & v = *tuple->values[j];
                    const bool null = Native::Sql::isNullable(v) && static_cast<const Native::Sql::NullableValue &>(v).isNull();
                    values.push_back(null ? nullLiteral : Native::Sql::toString(Native::Sql::getAssociatedValue(v)));
                    key += null ? std::string("N") : "V" + std::to_string(values.back().size()) + ":" + values.back();
                }
                auto it = index.find(key);
                if (it == index.end()) {
                    it = index.emplace(key, groups.size()).first;
                    groups.push_back({std::move(values), std::vector<Accumulator>(plan.aggregates.size())});
                }
                Group & group = groups[it->second];
                for (size_t k = 0; k < plan.aggregates.size(); ++k) {
                    const Native::Sql::Value * value = nullptr;
                    if (argumentColumn[k] >= 0) {
                        const Native::Sql::Value & v = *tuple->values[argumentColumn[k]];
                        if (!(Native::Sql::isNullable(v) && static_cast<const Native::Sql::NullableValue &>(v).isNull())) value = &v;
                    }
                    accumulate(plan.aggregates[k], argumentColumn[k] >= 0 ? rowTypes[argumentColumn[k]] : Sql::getUnknownTy(), group.accumulators[k], value);
                }
            }, [&](const StatementResult & d) { rowTypes = d.columnTypes; }, describeOnly);
        }
        // without GROUP BY, one group, also of no rows
        if (plan.groups.empty() && groups.empty()) groups.push_back({{}, std::vector<Accumulator>(plan.aggregates.size())});

        // the groups and their aggregates
        std::vector<std::pair<std::string, Sql::SqlType>> columns;
        std::vector<Sql::SqlType> inputTypes;
        // strings as text, which owns its characters (see with::columnType)
        for (size_t j = 0; j < plan.groups.size(); ++j) columns.emplace_back("_g" + std::to_string(j), with::columnType(rowTypes[j]));
        for (size_t k = 0; k < plan.aggregates.size(); ++k) {
            inputTypes.push_back(argumentColumn[k] >= 0 ? rowTypes[argumentColumn[k]] : Sql::getUnknownTy());
            columns.emplace_back("_a" + std::to_string(k), resultType(plan.aggregates[k], inputTypes.back()));
        }
        static std::atomic<uint64_t> tables(0);
        const std::string name = "\x01" "aggregate" + std::to_string(++tables);
        Table * table;
        {
            ModuleGen moduleGen("TemporaryTable"); // the sizes of the columns take the data layout
            table = &temporaries.create(columns);
        }
        if (!describeOnly) {
            QueryContext insertContext(db);
            insertContext.executionContext.branchId = master_branch_id;
            for (auto & group : groups) {
                std::vector<Native::Sql::value_op_t> values;
                for (size_t j = 0; j < plan.groups.size(); ++j) values.push_back(Native::Sql::castColumnValue(group.values[j], columns[j].second));
                for (size_t k = 0; k < plan.aggregates.size(); ++k) {
                    const auto type = columns[plan.groups.size() + k].second;
                    values.push_back(Native::Sql::castColumnValue(finish(plan.aggregates[k], inputTypes[k], type, group.accumulators[k]), type));
                }
                Native::Sql::SqlTuple tuple(std::move(values));
                insert_tuple(tuple, *table, insertContext);
            }
        }
        temporaries.bind(name, table);

        // the select list over them
        auto output = std::make_unique<semanticalAnalysis::SelectStatement>();
        semanticalAnalysis::Relation relation;
        relation.name = name;
        relation.version = "master";
        output->relations.push_back(relation);
        output->projections = plan.outputs;
        QueryContext context(db);
        context.analyzingContext.parserResult.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
        context.analyzingContext.parserResult.selectStmt = output.release();
        StatementResult result = runSelect(context, "aggregate", db, branch, transaction, temporaries, sink, described, describeOnly);
        temporaries.bind(name, nullptr);
        temporaries.release(table);
        return result;
    }

    /// Runs a parsed query (relations of temporary tables already in master) under the latches of its statement
    static StatementResult runSelect(QueryContext & queryContext, const std::string & text, Database & db, const std::string & branch,
                                     const Transaction * transaction, TemporaryTables & temporaries,
                                     const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                     const std::function<void(const StatementResult &)> & described, bool describeOnly) {
        queryContext.tupleSink = sink; // the translation streams the tuples to a sink instead of printing them
        auto & parserResult = queryContext.analyzingContext.parserResult;
        std::optional<ModuleGen> moduleGen;
        moduleGen.emplace("QueryModule");
        provideOneRowTable(db, temporaries, parserResult);
        auto analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        try {
            analyser->verify();
        } catch (const semanticalAnalysis::general_aggregation_required &) {
            moduleGen.reset(); // the queries of the aggregation generate their own modules
            return runAggregation(*parserResult.selectStmt, db, branch, transaction, temporaries, sink, described, describeOnly);
        }
        analyser->constructTree();
        auto & queryTree = queryContext.analyzingContext.joinedTree;
        auto * root = (queryTree != nullptr) ? dynamic_cast<Algebra::Logical::Result *>(queryTree->getRoot()) : nullptr;
        if (root == nullptr) throw semanticalAnalysis::semantic_sql_error("WITH, UNION and VALUES combine queries");
        StatementResult result;
        result.tag = "SELECT";
        describeResult(*root, parserResult, result);
        if (described) described(result);
        if (describeOnly) return result;

        auto queryFunc = compileQuery(text, queryTree, queryContext);
        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds, queryContext);
        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;
        uint64_t rows = 0;
        // workers of parallel scans call the sink under the result latch: an exception must not leave it (the
        // generated code would not release the latch); the first one ends the statement after the scan
        std::exception_ptr failure;
        queryContext.tupleSink = [&rows, &sink, &failure](Native::Sql::SqlTuple * tuple) {
            if (failure) return;
            try {
                ++rows;
                sink(tuple);
            } catch (...) {
                failure = std::current_exception();
            }
        };
        QueryExecutor::executeFunction(queryFunc, args, (void *) &printFunction);
        if (failure) std::rethrow_exception(failure);
        result.rows = rows;
        return result;
    }

    /// Runs a query of a WITH statement or of a UNION under the latches of the statement: relations named
    /// as temporary tables (the common table expressions) are read in the master branch
    static StatementResult runQuery(const std::string & text, Database & db, const std::string & branch, const Transaction * transaction,
                                    TemporaryTables & temporaries,
                                    const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                    const std::function<void(const StatementResult &)> & described,
                                    const std::string & recursiveName = std::string(), bool describeOnly = false) {
        QueryContext queryContext(db);
        const std::string defaultBranch = (transaction != nullptr) ? transaction->snapshot : branch;
        queryContext.parsingContext.defaultBranch = defaultBranch;
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, text + ";");
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult, queryContext.parsingContext);
        auto & parserResult = queryContext.analyzingContext.parserResult;
        if (parserResult.opType != semanticalAnalysis::SQLParserResult::OpType::Select) {
            throw semanticalAnalysis::semantic_sql_error("WITH, UNION and VALUES combine queries");
        }
        size_t recursiveReferences = 0;
        for (auto & relation : parserResult.selectStmt->relations) {
            if (!recursiveName.empty() && relation.name == recursiveName) ++recursiveReferences;
            if (relation.name == sqlExpressions::oneRowTable || TemporaryTables::find(db, relation.name) == nullptr) continue;
            if (!relation.versionScope.empty() || relation.version != defaultBranch) {
                throw semanticalAnalysis::semantic_sql_error("the common table expression '" + relation.name + "' has no versions");
            }
            relation.version = "master";
        }
        if (recursiveReferences > 1) {
            throw semanticalAnalysis::semantic_sql_error("recursive reference to query \"" + recursiveName + "\" must not appear more than once");
        }
        if (transaction != nullptr) confine(parserResult, *transaction, db);
        return runSelect(queryContext, text, db, branch, transaction, temporaries, sink, described, describeOnly);
    }

    /// WITH [ RECURSIVE ], UNION, INTERSECT and EXCEPT [ ALL ], and VALUES. Each common table expression
    /// is computed into a temporary table before the queries that follow read it. A recursive one (as in
    /// PostgreSQL: non-recursive query UNION [ ALL ] a SELECT that reads it) first computes the
    /// non-recursive query; then the SELECT reads the rows the last step added (the working table), until
    /// a step adds none. UNION without ALL adds no row twice.
    static StatementResult executeSetStatement(with::WithStatement & statement, Database & db, const std::string & branch,
                                               const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                               const std::function<void(const StatementResult &)> & described,
                                               const Transaction * transaction) {
        using semanticalAnalysis::semantic_sql_error;
        using with::SetQuery;
        using with::Row;
        const std::string defaultBranch = (transaction != nullptr) ? transaction->snapshot : branch;
        std::function<void(SetQuery &, std::vector<SetQuery *> &)> terms = [&](SetQuery & query, std::vector<SetQuery *> & out) {
            if (query.isTerm()) {
                out.push_back(&query);
            } else {
                terms(*query.left, out);
                terms(*query.right, out);
            }
        };
        auto termsOf = [&](SetQuery & query) {
            std::vector<SetQuery *> out;
            terms(query, out);
            return out;
        };
        auto reads = [&](SetQuery & query, const std::string & name) {
            for (SetQuery * term : termsOf(query)) if (term->relations.count(name) > 0) return true;
            return false;
        };

        // the relations of all queries, latched for the whole statement
        semanticalAnalysis::SQLParserResult relations;
        relations.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
        relations.selectStmt = new semanticalAnalysis::SelectStatement();
        auto collect = [&](SetQuery & query) {
            for (SetQuery * term : termsOf(query)) {
                QueryContext context(db);
                context.parsingContext.defaultBranch = defaultBranch;
                tardisParser::SQLParser::parseStatement(context.parsingContext, term->term + ";");
                QueryContext::convertToParserResult(context.analyzingContext.parserResult, context.parsingContext);
                auto & parsed = context.analyzingContext.parserResult;
                if (parsed.opType != semanticalAnalysis::SQLParserResult::OpType::Select) {
                    throw semantic_sql_error("WITH, UNION, INTERSECT, EXCEPT and VALUES combine queries");
                }
                for (auto & relation : parsed.selectStmt->relations) {
                    term->relations.insert(relation.name);
                    if (relation.name != sqlExpressions::oneRowTable) relations.selectStmt->relations.push_back(relation);
                }
            }
        };
        for (auto & cte : statement.ctes) collect(*cte.body);
        collect(*statement.query);
        StatementLatches latches(db, relations, transaction);
        std::set<branch_id_t> branches;
        for (auto & relation : relations.selectStmt->relations) {
            auto add = [&](const std::string & version) {
                auto it = db._branchMapping.find(version);
                if (it != db._branchMapping.end()) branches.insert(it->second);
            };
            if (relation.versionScope == "all" || relation.versionScope == "descendants" || relation.versionScope == "leaves") {
                for (auto & [id, b] : db._branches) branches.insert(id);
            }
            add(relation.version);
            for (auto & version : relation.versionSet) add(version);
        }
        latches.latchReadBranches(db, branches);

        TemporaryTables temporaries(db);
        QueryContext insertContext(db);
        insertContext.executionContext.branchId = master_branch_id;
        // the names (of the first SELECT) and the types (of all) of the columns, before any query runs
        auto describe = [&](SetQuery & query, std::vector<std::string> & names) {
            std::vector<Sql::SqlType> types;
            bool first = true;
            for (SetQuery * term : termsOf(query)) {
                auto d = runQuery(term->term, db, branch, transaction, temporaries, nullptr, nullptr, std::string(), true);
                if (first) {
                    names = d.columnNames;
                    for (auto & type : d.columnTypes) types.push_back(with::columnType(type));
                    first = false;
                } else if (d.columnTypes.size() != types.size()) {
                    throw semantic_sql_error("each query of a set operation has the same number of columns");
                } else {
                    for (size_t c = 0; c < types.size(); ++c) types[c] = with::unifyTypes(types[c], d.columnTypes[c], c);
                }
            }
            return types;
        };
        // the rows of a query in the types
        std::function<std::vector<Row>(SetQuery &, const std::vector<Sql::SqlType> &)> evaluate =
                [&](SetQuery & query, const std::vector<Sql::SqlType> & types) {
            if (!query.isTerm()) {
                auto left = evaluate(*query.left, types);
                auto right = evaluate(*query.right, types);
                return with::combineRows(query.op, query.all, std::move(left), std::move(right));
            }
            std::vector<Row> rows;
            runQuery(query.term, db, branch, transaction, temporaries, [&](Native::Sql::SqlTuple * tuple) {
                Row row;
                row.values = with::convertRow(*tuple, types, row.key);
                rows.push_back(std::move(row));
            }, nullptr);
            return rows;
        };

        std::set<std::string> defined;
        for (auto & cte : statement.ctes) {
            if (!defined.insert(cte.name).second) throw semantic_sql_error("WITH query name \"" + cte.name + "\" specified more than once");
            SetQuery & body = *cte.body;
            SetQuery * nonRecursive = &body;
            SetQuery * recursive = nullptr;
            if (statement.recursive && reads(body, cte.name)) {
                if (body.op != "union" || !body.right->isTerm() || !reads(*body.right, cte.name) || reads(*body.left, cte.name)) {
                    throw semantic_sql_error("recursive query \"" + cte.name + "\" does not have the form non-recursive-term UNION [ALL] recursive-term");
                }
                nonRecursive = body.left.get();
                recursive = body.right.get();
            }

            // the columns, of the non-recursive query
            std::vector<std::string> names;
            const std::vector<Sql::SqlType> types = describe(*nonRecursive, names);
            if (!cte.columns.empty()) {
                if (cte.columns.size() != types.size()) {
                    throw semantic_sql_error("WITH query \"" + cte.name + "\" has " + std::to_string(types.size()) +
                                             " columns available but " + std::to_string(cte.columns.size()) + " columns specified");
                }
                names = cte.columns;
            }
            std::vector<std::pair<std::string, Sql::SqlType>> columns;
            std::set<std::string> unique;
            for (size_t c = 0; c < types.size(); ++c) {
                if (!unique.insert(names[c]).second) {
                    throw semantic_sql_error("column name \"" + names[c] + "\" of WITH query \"" + cte.name +
                                             "\" appears more than once; name the columns: " + cte.name + " ( ... )");
                }
                columns.emplace_back(names[c], types[c]);
            }
            // the recursive query gives the same types, as in PostgreSQL
            auto describeRecursive = [&](const StatementResult & d) {
                if (d.columnTypes.size() != types.size()) throw semantic_sql_error("each query of a set operation has the same number of columns");
                for (size_t c = 0; c < types.size(); ++c) {
                    if (!Sql::equals(with::unifyTypes(types[c], d.columnTypes[c], c), types[c], Sql::SqlTypeEqualsMode::Full)) {
                        throw semantic_sql_error("recursive query \"" + cte.name + "\" column " + std::to_string(c + 1) + " has type " +
                                                 Sql::getName(types[c]) + " in non-recursive term but type " +
                                                 Sql::getName(with::columnType(d.columnTypes[c])) + " overall");
                    }
                }
            };

            Table * result = nullptr; // all rows
            Table * next = nullptr;   // the rows of the current step, which the next one reads
            {
                ModuleGen moduleGen("TemporaryTable"); // the sizes of the columns take the data layout
                result = &temporaries.create(columns);
                if (recursive != nullptr) next = &temporaries.create(columns);
            }
            // UNION without ALL: no row twice, neither in a step nor among the steps
            const bool removeDuplicates = recursive != nullptr && !body.all;
            std::unordered_set<std::string> seen;
            uint64_t added = 0;
            auto insert = [&](Row & row) {
                if (removeDuplicates && !seen.insert(row.key).second) return;
                Native::Sql::SqlTuple tuple(std::move(row.values));
                insert_tuple(tuple, *result, insertContext);
                if (next != nullptr) insert_tuple(tuple, *next, insertContext);
                ++added;
            };
            for (auto & row : evaluate(*nonRecursive, types)) insert(row);
            if (recursive != nullptr) {
                do {
                    Table * working = next;
                    {
                        ModuleGen moduleGen("TemporaryTable");
                        next = &temporaries.create(columns);
                    }
                    temporaries.bind(cte.name, working);
                    added = 0;
                    runQuery(recursive->term, db, branch, transaction, temporaries, [&](Native::Sql::SqlTuple * tuple) {
                        Row row;
                        row.values = with::convertRow(*tuple, types, row.key);
                        insert(row);
                    }, describeRecursive, cte.name);
                    temporaries.bind(cte.name, nullptr);
                    temporaries.release(working);
                } while (added > 0);
                temporaries.release(next);
                next = nullptr;
            }
            temporaries.bind(cte.name, result);
        }

        SetQuery & query = *statement.query;
        if (query.isTerm()) {
            return runQuery(query.term, db, branch, transaction, temporaries, sink, described);
        }
        // the rows of a set operation, in the types of all its queries
        StatementResult result;
        result.tag = "SELECT";
        result.returnsRows = true;
        result.columnTypes = describe(query, result.columnNames);
        if (described) described(result);
        for (auto & row : evaluate(query, result.columnTypes)) {
            Native::Sql::SqlTuple tuple(std::move(row.values));
            ++result.rows;
            sink(&tuple);
        }
        return result;
    }

    /// \returns The query without the semicolons and spaces at its end
    static std::string withoutEnd(std::string query) {
        while (!query.empty() && (query.back() == ';' || std::isspace(static_cast<unsigned char>(query.back())))) query.pop_back();
        return query;
    }

    static StatementResult executeStatement(const std::string & query, Database & db, const std::string & branch,
                                            const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                            const std::function<void(const StatementResult &)> & described,
                                            const Transaction * transaction) {
        QueryContext queryContext(db);
        queryContext.tupleSink = sink;
        ModuleGen moduleGen("QueryModule");

        queryContext.parsingContext.defaultBranch = (transaction != nullptr) ? transaction->snapshot : branch;
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult, queryContext.parsingContext);
        StatementLatches latches(db, queryContext.analyzingContext.parserResult, transaction);
        std::optional<TemporaryTables> temporaries; // under the latches: no branch is created meanwhile
        if (readsOneRow(queryContext.analyzingContext.parserResult)) {
            temporaries.emplace(db);
            provideOneRowTable(db, *temporaries, queryContext.analyzingContext.parserResult);
        }

        auto & parserResult = queryContext.analyzingContext.parserResult;
        StatementResult result;
        result.tag = statementTag(parserResult.opType);
        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        analyser->verify();
        analyser->constructTree();
        auto & queryTree = queryContext.analyzingContext.joinedTree;
        if (queryTree == nullptr) {
            result.rows = queryContext.analyzingContext.changedTuples;
            result.notice = queryContext.analyzingContext.notice;
            return result;
        }
        latches.latchReadBranches(db, queryContext.analyzingContext.branchIds);

        // the columns of the result, named as in the select list (aggregates by their function)
        if (auto * root = dynamic_cast<Algebra::Logical::Result *>(queryTree->getRoot())) {
            describeResult(*root, parserResult, result);
        }

        if (described) described(result);

        auto queryFunc = compileQuery(query, queryTree, queryContext);
        if (queryFunc == nullptr) return result;
        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds, queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;
        uint64_t rows = 0;
        std::exception_ptr failure;
        if (result.returnsRows) {
            // count the tuples on their way to the sink; an exception of the sink must not leave the result
            // latch of a parallel scan, so the first one ends the statement after the scan
            queryContext.tupleSink = [&rows, &sink, &failure](Native::Sql::SqlTuple * tuple) {
                if (failure) return;
                try {
                    ++rows;
                    sink(tuple);
                } catch (...) {
                    failure = std::current_exception();
                }
            };
        }
        QueryExecutor::executeFunction(queryFunc, args, (void *) &printFunction);
        if (failure) std::rethrow_exception(failure);
        result.rows = result.returnsRows ? rows : queryContext.executionContext.affectedRows;
        return result;
    }

    StatementResult execute(const std::string & query, Database & db, const std::string & branch,
                            const std::function<void(Native::Sql::SqlTuple *)> & sink,
                            const std::function<void(const StatementResult &)> & described,
                            const Transaction * transaction) {
        with::WithStatement setStatement;
        if (with::parseSetStatement(query, setStatement)) {
            return executeSetStatement(setStatement, db, branch, sink, described, transaction);
        }
        try {
            return executeStatement(query, db, branch, sink, described, transaction);
        } catch (const semanticalAnalysis::general_aggregation_required &) {
            // the latches and the module of the statement are gone: its query runs as the queries of WITH
            with::WithStatement statement;
            statement.query = with::makeTerm(withoutEnd(query));
            return executeSetStatement(statement, db, branch, sink, described, transaction);
        }
    }

    BenchmarkResult compileAndBenchmark(const std::string &query, Database &db, void *callbackFunction) {
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

        const auto parsingStart = std::chrono::high_resolution_clock::now();
#if USE_HYRISE
        hsql::SQLParser::parse(query, &queryContext.hyriseResult);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.hyriseResult);
#else
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.parsingContext);
#endif
        const auto parsingDuration = std::chrono::high_resolution_clock::now() - parsingStart;
        StatementLatches latches(db, queryContext.analyzingContext.parserResult);

        const auto analysingStart = std::chrono::high_resolution_clock::now();
        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        analyser->verify();
        analyser->constructTree();
        auto &queryTree = queryContext.analyzingContext.joinedTree;
        const auto analysingDuration = std::chrono::high_resolution_clock::now() - analysingStart;
        if (queryTree == nullptr) return BenchmarkResult();
        latches.latchReadBranches(db, queryContext.analyzingContext.branchIds);
        
        // return columns
        BenchmarkResult result;
        result.columns = queryTree->getRoot()->getRequired();

        const auto translationStart = std::chrono::high_resolution_clock::now();
        auto queryFunc = compileQuery(query, queryTree, queryContext);
        const auto translationDuration = std::chrono::high_resolution_clock::now() - translationStart;
        if (queryFunc == nullptr) unreachable();

        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds,queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::BenchmarkResult llvmresult = QueryExecutor::executeBenchmarkFunction(queryFunc, args, 1, callbackFunction);

        result.parsingTime = std::chrono::duration_cast<std::chrono::microseconds>(parsingDuration).count();
        result.analysingTime = std::chrono::duration_cast<std::chrono::microseconds>(analysingDuration).count();
        result.translationTime = std::chrono::duration_cast<std::chrono::microseconds>(translationDuration).count();
        result.llvmCompilationTime = llvmresult.compilationTime;
        result.executionTime = llvmresult.executionTime;

        return result;
    }

} // end namespace QueryCompiler
