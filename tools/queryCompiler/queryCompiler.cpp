//
// Created by josef on 06.01.17.
//

#include "queryCompiler/queryCompiler.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
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

    void compileAndExecute(const std::string &query, Database &db, void *callbackFunction) {
        {
            // WITH, UNION, VALUES and SELECT without FROM run as the statements of a session
            std::string lower;
            std::transform(query.begin(), query.end(), std::back_inserter(lower), ::tolower);
            size_t first = lower.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && (lower.compare(first, 4, "with") == 0 || lower.compare(first, 6, "values") == 0 ||
                lower.compare(first, 1, "(") == 0 || (lower.compare(first, 6, "select") == 0 &&
                (lower.find("union") != std::string::npos || lower.find(" from ") == std::string::npos)))) {
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

    /// Queries combined by UNION [ ALL ], from left to right
    struct SetQuery {
        std::vector<std::string> terms; // the text of each SELECT
        std::vector<bool> all;          // UNION ALL before the term (not UNION); the first term's is false
    };

    struct CommonTableExpression {
        std::string name;
        std::vector<std::string> columns; // empty: the names of the first query's columns
        SetQuery body;
    };

    /// [ WITH [ RECURSIVE ] name [ ( columns ) ] AS ( query ) [ , ... ] ] query
    struct WithStatement {
        bool recursive = false;
        std::vector<CommonTableExpression> ctes;
        SetQuery query;
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

    /// The queries of [from, to), combined by UNION [ ALL | DISTINCT ]; VALUES ( ... ) [ , ... ] is a
    /// SELECT per row, with the columns column1, column2, ... as in PostgreSQL
    static SetQuery splitSetQuery(const std::string & sql, const std::vector<Lexeme> & lexemes, size_t from, size_t to) {
        SetQuery query;
        bool all = false;
        auto addTerm = [&](size_t a, size_t b) {
            // ( SELECT ... ) without its parentheses
            while (b > a + 1 && lexemes[a].kind == Lexeme::Open && matching(lexemes, a, b) == b - 1) { ++a; --b; }
            if (a >= b) throw syntaxError("a query of UNION is missing");
            if (!isWord(lexemes, a, "values")) {
                query.terms.push_back(sql.substr(lexemes[a].begin, lexemes[b - 1].end - lexemes[a].begin));
                query.all.push_back(all);
                return;
            }
            // the rows after the first are UNION ALL, unless a later UNION removes duplicates anyway
            const bool rowsAll = query.terms.empty() ? true : all;
            bool first = true;
            for (size_t i = a + 1; i < b; ) {
                if (lexemes[i].kind != Lexeme::Open) throw syntaxError("Expected '(' of a row of VALUES");
                size_t close = matching(lexemes, i, b);
                auto values = splitItems(sql, lexemes, i + 1, close);
                std::string term = "SELECT ";
                for (size_t v = 0; v < values.size(); ++v) {
                    term += (v > 0 ? ", " : "") + values[v] + " AS column" + std::to_string(v + 1);
                }
                query.terms.push_back(term);
                query.all.push_back(first ? all : rowsAll);
                first = false;
                i = close + 1;
                if (i < b) {
                    if (lexemes[i].kind != Lexeme::Comma) throw syntaxError("Expected ',' between the rows of VALUES");
                    ++i;
                }
            }
            if (first) throw syntaxError("VALUES without rows");
        };
        size_t depth = 0, start = from;
        for (size_t i = from; i < to; ++i) {
            const Lexeme & lexeme = lexemes[i];
            if (lexeme.kind == Lexeme::Open) {
                ++depth;
            } else if (lexeme.kind == Lexeme::Close) {
                if (depth == 0) throw syntaxError("unbalanced parentheses");
                --depth;
            } else if (depth == 0 && lexeme.kind == Lexeme::Word && (lexeme.word == "intersect" || lexeme.word == "except")) {
                throw semanticalAnalysis::semantic_sql_error(lexeme.word + " is not supported (only UNION and UNION ALL)");
            } else if (depth == 0 && lexeme.kind == Lexeme::Word && lexeme.word == "union") {
                addTerm(start, i);
                start = i + 1;
                all = isWord(lexemes, start, "all");
                if (all || isWord(lexemes, start, "distinct")) ++start;
            }
        }
        if (depth != 0) throw syntaxError("unbalanced parentheses");
        addTerm(start, to);
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

    /// \returns Whether execute() runs the statement here: WITH, queries combined by UNION, or VALUES
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
            if (lower.find("union") == std::string::npos) return false;
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
                else if (depth == 0 && lexeme.kind == Lexeme::Word && lexeme.word == "union") setQuery = true;
            }
            if (!setQuery) return false;
            statement.query = splitSetQuery(sql, lexemes, 0, n);
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
            cte.body = splitSetQuery(sql, lexemes, i + 1, close);
            i = close + 1;
            statement.ctes.push_back(std::move(cte));
            if (i < n && lexemes[i].kind == Lexeme::Comma) {
                ++i;
                continue;
            }
            break;
        }
        if (i >= n) throw syntaxError("Expected a query after WITH");
        statement.query = splitSetQuery(sql, lexemes, i, n);
        return true;
    }

    /// \returns Per query, whether its rows are free of duplicates: up to the last UNION without ALL, since
    /// UNION combines from left to right
    static std::vector<bool> distinctTerms(const SetQuery & query) {
        std::vector<bool> distinct(query.terms.size(), false);
        for (size_t t = query.terms.size(); t-- > 1; ) {
            if (!query.all[t]) {
                std::fill(distinct.begin(), distinct.begin() + t + 1, true);
                break;
            }
        }
        return distinct;
    }

    /// The type of a column of a common table expression or of UNION: nullable, and text for all strings,
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
            throw semanticalAnalysis::semantic_sql_error("each query of UNION has the same number of columns");
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

    /// Runs a query of a WITH statement or of a UNION under the latches of the statement: relations named
    /// as temporary tables (the common table expressions) are read in the master branch
    static StatementResult runQuery(const std::string & text, Database & db, const std::string & branch, const Transaction * transaction,
                                    TemporaryTables & temporaries,
                                    const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                    const std::function<void(const StatementResult &)> & described,
                                    const std::string & recursiveName = std::string(), bool describeOnly = false) {
        QueryContext queryContext(db);
        queryContext.tupleSink = sink; // the translation streams the tuples to a sink instead of printing them
        ModuleGen moduleGen("QueryModule");
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
        provideOneRowTable(db, temporaries, parserResult);
        if (transaction != nullptr) confine(parserResult, *transaction, db);

        auto analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        analyser->verify();
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
        queryContext.tupleSink = [&rows, &sink](Native::Sql::SqlTuple * tuple) { ++rows; sink(tuple); };
        QueryExecutor::executeFunction(queryFunc, args, (void *) &printFunction);
        result.rows = rows;
        return result;
    }

    /// WITH [ RECURSIVE ], UNION [ ALL ] and VALUES. Each common table expression is computed into a
    /// temporary table before the queries that follow read it. A recursive one (a query of its UNION reads
    /// it) first computes its queries that do not read it; then its other queries read the rows the last
    /// step added (the working table), until a step adds none. UNION without ALL adds no row twice.
    static StatementResult executeSetStatement(const with::WithStatement & statement, Database & db, const std::string & branch,
                                               const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                               const std::function<void(const StatementResult &)> & described,
                                               const Transaction * transaction) {
        using semanticalAnalysis::semantic_sql_error;
        const std::string defaultBranch = (transaction != nullptr) ? transaction->snapshot : branch;

        // the relations of all queries, latched for the whole statement
        semanticalAnalysis::SQLParserResult reads;
        reads.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
        reads.selectStmt = new semanticalAnalysis::SelectStatement();
        std::vector<std::set<std::string>> termRelations;
        auto collect = [&](const with::SetQuery & query) {
            for (auto & text : query.terms) {
                QueryContext context(db);
                context.parsingContext.defaultBranch = defaultBranch;
                tardisParser::SQLParser::parseStatement(context.parsingContext, text + ";");
                QueryContext::convertToParserResult(context.analyzingContext.parserResult, context.parsingContext);
                auto & parsed = context.analyzingContext.parserResult;
                if (parsed.opType != semanticalAnalysis::SQLParserResult::OpType::Select) {
                    throw semantic_sql_error("WITH, UNION and VALUES combine queries");
                }
                std::set<std::string> names;
                for (auto & relation : parsed.selectStmt->relations) {
                    names.insert(relation.name);
                    if (relation.name != sqlExpressions::oneRowTable) reads.selectStmt->relations.push_back(relation);
                }
                termRelations.push_back(std::move(names));
            }
        };
        for (auto & cte : statement.ctes) collect(cte.body);
        collect(statement.query);
        StatementLatches latches(db, reads, transaction);
        std::set<branch_id_t> branches;
        for (auto & relation : reads.selectStmt->relations) {
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
        // the names and types of the columns of queries combined by UNION, before any of them runs
        auto describeAll = [&](const std::vector<std::string> & terms, size_t count, std::vector<std::string> & names) {
            std::vector<Sql::SqlType> types;
            for (size_t t = 0; t < count; ++t) {
                auto d = runQuery(terms[t], db, branch, transaction, temporaries, nullptr, nullptr, std::string(), true);
                if (t == 0) {
                    names = d.columnNames;
                    for (auto & type : d.columnTypes) types.push_back(with::columnType(type));
                } else if (d.columnTypes.size() != types.size()) {
                    throw semantic_sql_error("each query of UNION has the same number of columns");
                } else {
                    for (size_t c = 0; c < types.size(); ++c) types[c] = with::unifyTypes(types[c], d.columnTypes[c], c);
                }
            }
            return types;
        };

        std::set<std::string> defined;
        size_t termIndex = 0;
        for (auto & cte : statement.ctes) {
            if (!defined.insert(cte.name).second) throw semantic_sql_error("WITH query name \"" + cte.name + "\" specified more than once");
            const auto & terms = cte.body.terms;
            const auto distinct = with::distinctTerms(cte.body);
            std::vector<bool> recursive(terms.size());
            for (size_t t = 0; t < terms.size(); ++t) {
                recursive[t] = statement.recursive && termRelations[termIndex + t].count(cte.name) > 0;
            }
            termIndex += terms.size();
            size_t anchors = 0;
            while (anchors < terms.size() && !recursive[anchors]) ++anchors;
            if (anchors == 0 || std::find(recursive.begin() + anchors, recursive.end(), false) != recursive.end()) {
                throw semantic_sql_error("recursive query \"" + cte.name + "\" does not have the form non-recursive-term UNION [ALL] recursive-term");
            }

            // the columns, of the queries that do not read the expression
            std::vector<std::string> names;
            const std::vector<Sql::SqlType> types = describeAll(terms, anchors, names);
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
            // the recursive queries give the same types, as in PostgreSQL
            auto describeRecursive = [&](const StatementResult & d) {
                if (d.columnTypes.size() != types.size()) throw semantic_sql_error("each query of UNION has the same number of columns");
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
                if (anchors < terms.size()) next = &temporaries.create(columns);
            }
            std::unordered_set<std::string> seen;
            uint64_t added = 0;
            auto insert = [&](Native::Sql::SqlTuple * tuple, bool removeDuplicates) {
                std::string key;
                auto values = with::convertRow(*tuple, types, key);
                if (removeDuplicates && !seen.insert(key).second) return;
                Native::Sql::SqlTuple row(std::move(values));
                insert_tuple(row, *result, insertContext);
                if (next != nullptr) insert_tuple(row, *next, insertContext);
                ++added;
            };
            for (size_t t = 0; t < anchors; ++t) {
                runQuery(terms[t], db, branch, transaction, temporaries,
                         [&](Native::Sql::SqlTuple * tuple) { insert(tuple, distinct[t]); }, nullptr);
            }
            if (anchors < terms.size()) {
                do {
                    Table * working = next;
                    {
                        ModuleGen moduleGen("TemporaryTable");
                        next = &temporaries.create(columns);
                    }
                    temporaries.bind(cte.name, working);
                    added = 0;
                    for (size_t t = anchors; t < terms.size(); ++t) {
                        runQuery(terms[t], db, branch, transaction, temporaries,
                                 [&](Native::Sql::SqlTuple * tuple) { insert(tuple, distinct[t]); }, describeRecursive, cte.name);
                    }
                    temporaries.bind(cte.name, nullptr);
                    temporaries.release(working);
                } while (added > 0);
                temporaries.release(next);
                next = nullptr;
            }
            temporaries.bind(cte.name, result);
        }

        const auto & terms = statement.query.terms;
        if (terms.size() == 1) {
            return runQuery(terms[0], db, branch, transaction, temporaries, sink, described);
        }
        // the rows of UNION, in the types of all its queries
        const auto distinct = with::distinctTerms(statement.query);
        StatementResult result;
        result.tag = "SELECT";
        result.returnsRows = true;
        result.columnTypes = describeAll(terms, terms.size(), result.columnNames);
        if (described) described(result);
        std::unordered_set<std::string> seen;
        for (size_t t = 0; t < terms.size(); ++t) {
            runQuery(terms[t], db, branch, transaction, temporaries, [&](Native::Sql::SqlTuple * tuple) {
                std::string key;
                auto values = with::convertRow(*tuple, result.columnTypes, key);
                if (distinct[t] && !seen.insert(key).second) return;
                Native::Sql::SqlTuple row(std::move(values));
                ++result.rows;
                sink(&row);
            }, nullptr);
        }
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
        if (result.returnsRows) {
            // count the tuples on their way to the sink
            queryContext.tupleSink = [&rows, &sink](Native::Sql::SqlTuple * tuple) { ++rows; sink(tuple); };
        }
        QueryExecutor::executeFunction(queryFunc, args, (void *) &printFunction);
        result.rows = result.returnsRows ? rows : queryContext.executionContext.affectedRows;
        return result;
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
