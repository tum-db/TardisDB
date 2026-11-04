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
        StatementLatches(Database & db, const semanticalAnalysis::SQLParserResult & statement) {
            using OpType = semanticalAnalysis::SQLParserResult::OpType;
            if (latching == Latching::Serial) {
                _serial = std::unique_lock<std::mutex>(serialLatch);
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
                    return;
            }
            _sharedCatalog = std::shared_lock<SharedLatch>(db._catalogLatch);

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

    } // end anonymous namespace

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
