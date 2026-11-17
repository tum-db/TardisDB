//
// Created by josef on 06.01.17.
//

#include "queryCompiler/queryCompiler.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <shared_mutex>

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

    StatementResult execute(const std::string & query, Database & db, const std::string & branch,
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
            result.returnsRows = true;
            const semanticalAnalysis::SelectStatement * select =
                    (parserResult.opType == semanticalAnalysis::SQLParserResult::OpType::Select) ? parserResult.selectStmt : nullptr;
            bool aligned = select != nullptr && std::none_of(select->projections.begin(), select->projections.end(),
                    [](const semanticalAnalysis::Column & c) { return c.name == "*" && c.aggregate.empty(); });
            size_t position = 0;
            for (iu_p_t iu : root->selection) {
                if (iu->iuType == InformationUnit::Type::ColumnRef && getColumnInformation(iu)->columnName == "tid") {
                    continue; // TupleStream leaves it out
                }
                std::string name = "?column?";
                if (aligned && position < select->projections.size()) {
                    auto & projection = select->projections[position];
                    name = projection.aggregate.empty() ? projection.name : projection.aggregate;
                } else if (iu->iuType == InformationUnit::Type::ColumnRef) {
                    name = getColumnInformation(iu)->columnName;
                }
                result.columnNames.push_back(name);
                result.columnTypes.push_back(iu->sqlType);
                ++position;
            }
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
