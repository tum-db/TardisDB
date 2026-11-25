//
// Created by josef on 06.01.17.
//

#include "queryCompiler/queryCompiler.hpp"
#include "queryCompiler/TableFunctions.hpp"
#include "semanticAnalyser/CopyFormat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdio>
#include <mutex>
#include <exception>
#include <optional>
#include <regex>
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
#include "algebra/physical/operators.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/version_management.hpp"
#include "queryExecutor/queryExecutor.hpp"

#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"
#include "native/sql/SqlTuple.hpp"
#include "native/sql/SqlValues.hpp"
#include "sql/SqlCasts.hpp"
#include "sqlParser/Expression.hpp"
#include "sqlParser/ParserResult.hpp"

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

    /// The times of the statement that runs on this thread (see Timing)
    namespace timing {

    using Clock = std::chrono::steady_clock;

    thread_local Timing current;
    thread_local int depth = 0;

    inline double since(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    /// Adds the time of its scope to a phase
    struct Phase {
        double & slot;
        Clock::time_point start = Clock::now();
        explicit Phase(double & slot) : slot(slot) { }
        ~Phase() { slot += since(start); }
    };

    /// A call of execute() or compileAndExecute(); the outermost one starts the times of the statement
    struct Statement {
        Clock::time_point start = Clock::now();
        bool outer;
        Statement() : outer(depth++ == 0) {
            if (outer) {
                current = Timing();
                QueryExecutor::runTimes() = QueryExecutor::RunTimes();
            }
        }
        ~Statement() { --depth; }
        Timing finish() const {
            Timing t = current;
            t.llvm = QueryExecutor::runTimes().compilation;
            t.total = since(start);
            t.execution = std::max(0.0, t.total - t.compilation());
            return t;
        }
    };

    } // end namespace timing

    /// EXPLAIN [ANALYZE]: the plans of the queries a statement compiles, as JSON
    namespace explain {

    namespace LE = Algebra::Logical::Expressions;

    /// An operator of a plan: its fields (rendered JSON values), its row counter and its inputs
    struct Node {
        std::vector<std::pair<std::string, std::string>> fields;
        const uint64_t * rows = nullptr;
        std::vector<Node> children;
    };

    struct Plan {
        std::string query;
        Node root;
        std::vector<std::unique_ptr<uint64_t>> counters; // addresses that the generated code embeds
    };

    struct State {
        bool analyze = false;
        std::vector<Plan> plans;
    };

    thread_local State * active = nullptr;

    /// The tables of the scans of the plan being described, by the scanUID of their columns; with
    /// more than one table, columns are qualified by their table
    thread_local std::unordered_map<uint32_t, std::string> scanTables;
    thread_local bool qualify = false;

    std::string quote(const std::string & s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        out += buffer;
                    } else {
                        out += c;
                    }
            }
        }
        return out + "\"";
    }

    std::string list(const std::vector<std::string> & items) {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + quote(items[i]);
        return out + "]";
    }

    std::string number(double value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", value);
        return buffer;
    }

    std::string tableName(Database & db, const Table & table) {
        for (auto & [name, t] : db.getTables()) if (t.get() == &table) return name;
        return "(temporary)"; // a common table expression, the groups of an aggregation, ...
    }

    std::string branchName(Database & db, branch_id_t id) {
        for (auto & [name, b] : db._branchMapping) if (b == id) return name;
        return std::to_string(id);
    }

    std::string iuName(iu_p_t iu) {
        if (iu != nullptr && iu->iuType == InformationUnit::Type::ColumnRef && iu->columnInformation != nullptr) {
            auto table = scanTables.find(iu->scanUID);
            if (qualify && table != scanTables.end()) return table->second + "." + iu->columnInformation->columnName;
            return iu->columnInformation->columnName;
        }
        return "?column?";
    }

    std::string arrayFunctionName(Native::Sql::Arrays::Function function) {
        static const char * names[] = {"array", "subscript", "slice", "array_append", "array_prepend", "array_cat",
            "array_length", "array_lower", "array_upper", "array_ndims", "array_dims", "cardinality", "array_position",
            "array_positions", "array_remove", "array_replace", "array_fill", "array_to_string", "string_to_array",
            "trim_array", "@>", "<@", "&&", "any", "all", "=", "elementwise", "**", "kroneckerproduct", "transpose",
            "map", "softmax", "softmaxcce", "softmaxccep", "highestposition"};
        const auto i = static_cast<size_t>(function);
        return i < sizeof(names) / sizeof(names[0]) ? names[i] : "array_function";
    }

    /// \returns The expression in SQL
    std::string expression(const LE::Expression & e) {
        auto binary = [](const LE::BinaryOperator & b, const char * op) {
            return "(" + expression(b.getLeftChild()) + " " + op + " " + expression(b.getRightChild()) + ")";
        };
        if (auto * c = dynamic_cast<const LE::Comparison *>(&e)) {
            static const char * ops[] = {"<", "<=", "=", ">=", ">"};
            return binary(*c, ops[static_cast<int>(c->_mode)]);
        }
        if (auto * b = dynamic_cast<const LE::And *>(&e)) return binary(*b, "AND");
        if (auto * b = dynamic_cast<const LE::Or *>(&e)) return binary(*b, "OR");
        if (auto * b = dynamic_cast<const LE::Addition *>(&e)) return binary(*b, "+");
        if (auto * b = dynamic_cast<const LE::Subtraction *>(&e)) return binary(*b, "-");
        if (auto * b = dynamic_cast<const LE::Multiplication *>(&e)) return binary(*b, "*");
        if (auto * b = dynamic_cast<const LE::Division *>(&e)) return binary(*b, "/");
        if (auto * n = dynamic_cast<const LE::Not *>(&e)) return "(NOT " + expression(n->getChild()) + ")";
        if (auto * n = dynamic_cast<const LE::IsNull *>(&e)) return "(" + expression(n->getChild()) + (n->_negated ? " IS NOT NULL)" : " IS NULL)");
        if (auto * c = dynamic_cast<const LE::Cast *>(&e)) return expression(c->getChild()) + "::" + Sql::getName(Sql::toNotNullableTy(c->_destType));
        if (auto * i = dynamic_cast<const LE::Identifier *>(&e)) return iuName(i->_iu);
        if (auto * c = dynamic_cast<const LE::Constant *>(&e)) {
            const auto id = c->_type.typeID;
            const bool quoted = id == Sql::SqlType::TypeID::TextID || id == Sql::SqlType::TypeID::VarcharID || id == Sql::SqlType::TypeID::CharID ||
                                id == Sql::SqlType::TypeID::DateID || id == Sql::SqlType::TypeID::TimestampID || id == Sql::SqlType::TypeID::ArrayID;
            return quoted ? "'" + c->_value + "'" : c->_value;
        }
        if (dynamic_cast<const LE::NullConstant *>(&e)) return "NULL";
        if (auto * f = dynamic_cast<const LE::ArrayFunction *>(&e)) {
            std::string out = arrayFunctionName(f->_function) + "(";
            for (size_t i = 0; i < f->_arguments.size(); ++i) out += (i ? ", " : "") + expression(*f->_arguments[i]);
            return out + ")";
        }
        return "?";
    }

    /// The node of an operator and its inputs; with ANALYZE, a row counter per operator
    Node describe(Algebra::Physical::Operator & op, Database & db, Plan & plan) {
        namespace P = Algebra::Physical;
        namespace L = Algebra::Logical;
        Node node;
        auto field = [&](const std::string & key, const std::string & json) { node.fields.emplace_back(key, json); };
        auto type = [&](const std::string & name) { field("Node Type", quote(name)); };
        const L::Operator & logical = op.getLogicalOperator();

        if (dynamic_cast<P::TableScan *>(&op)) {
            auto & scan = const_cast<L::TableScan &>(dynamic_cast<const L::TableScan &>(logical));
            type(scan.getIndex() != nullptr ? "Index Scan" : "Table Scan");
            field("Relation Name", quote(tableName(db, scan.getTable())));
            field("Branch", quote(branchName(db, scan.getBranchId())));
            if (scan.getIndex() != nullptr) field("Index Name", quote(scan.getIndex()->name));
        } else if (dynamic_cast<P::Select *>(&op)) {
            type("Filter");
            field("Filter", quote(expression(*dynamic_cast<const L::Select &>(logical)._exp)));
        } else if (dynamic_cast<P::Map *>(&op)) {
            type("Map");
            std::vector<std::string> outputs;
            for (auto & mapping : dynamic_cast<const L::Map &>(logical)._mappings) outputs.push_back(expression(*mapping.exp));
            field("Output", list(outputs));
        } else if (dynamic_cast<P::HashJoin *>(&op) || dynamic_cast<P::MergeJoin *>(&op)) {
            const bool merge = dynamic_cast<P::MergeJoin *>(&op) != nullptr;
            type(merge ? "Merge Join" : "Hash Join");
            std::string condition;
            for (auto & e : dynamic_cast<const L::Join &>(logical)._joinExprVec) condition += (condition.empty() ? "" : " AND ") + expression(*e);
            if (!condition.empty()) field(merge ? "Merge Cond" : "Join Cond", quote(condition));
        } else if (dynamic_cast<P::GroupBy *>(&op)) {
            type("Aggregate");
            std::vector<std::string> aggregates;
            for (auto & a : dynamic_cast<const L::GroupBy &>(logical)._aggregations) {
                namespace A = L::Aggregations;
                aggregates.push_back(dynamic_cast<A::Sum *>(a.get()) ? "sum" : dynamic_cast<A::Avg *>(a.get()) ? "avg" :
                                     dynamic_cast<A::CountAll *>(a.get()) ? "count(*)" : dynamic_cast<A::Min *>(a.get()) ? "min" : "group key");
            }
            field("Aggregates", list(aggregates));
        } else if (dynamic_cast<P::BranchAggregation *>(&op)) {
            auto & aggregation = dynamic_cast<const L::BranchAggregation &>(logical);
            type("Branch Aggregation");
            field("Relation Name", quote(tableName(db, aggregation.getTable())));
            std::vector<std::string> branches;
            for (auto b : aggregation.getBranches()) branches.push_back(branchName(db, b));
            field("Branches", list(branches));
            field("Strategy", quote(L::BranchAggregation::_strategy == L::BranchAggregation::Strategy::Shared ? "branch-shared scan" : "scan per branch"));
            std::vector<std::string> aggregates;
            static const char * functions[] = {"count", "sum", "min", "max", "avg"};
            for (auto & a : aggregation.getAggregates()) {
                aggregates.push_back(std::string(functions[static_cast<int>(a.function)]) + "(" + (a.input ? iuName(a.input) : "*") + ")");
            }
            field("Aggregates", list(aggregates));
            std::string condition;
            for (auto & e : aggregation._predicates) condition += (condition.empty() ? "" : " AND ") + expression(*e);
            if (!condition.empty()) field("Filter", quote(condition));
        } else if (dynamic_cast<P::BranchDiff *>(&op)) {
            auto & diff = dynamic_cast<const L::BranchDiff &>(logical);
            type("Branch Diff");
            field("Relation Name", quote(tableName(db, diff.getTable())));
            field("Branches", list({branchName(db, diff.getFirst()), branchName(db, diff.getSecond())}));
            std::string condition;
            for (auto & e : diff._predicates) condition += (condition.empty() ? "" : " AND ") + expression(*e);
            if (!condition.empty()) field("Filter", quote(condition));
        } else if (dynamic_cast<P::Insert *>(&op)) {
            auto & insert = const_cast<L::Insert &>(dynamic_cast<const L::Insert &>(logical));
            type("Insert");
            field("Relation Name", quote(tableName(db, insert.getTable())));
            field("Branch", quote(branchName(db, insert.getBranchId())));
            field("Tuples", std::to_string(insert.getTuples().size()));
        } else if (auto * physicalUpdate = dynamic_cast<P::Update *>(&op)) {
            auto & update = const_cast<L::Update &>(dynamic_cast<const L::Update &>(logical));
            type("Update");
            field("Relation Name", quote(tableName(db, update.getTable())));
            field("Branch", quote(branchName(db, update.getBranchId())));
            std::vector<std::string> set; // the constants went to the physical operator
            for (auto & pair : physicalUpdate->getUpdateIUs()) if (!pair.second.empty()) set.push_back(iuName(pair.first) + " = " + pair.second);
            for (auto & pair : update.updateExpressions) set.push_back(iuName(pair.first) + " = " + expression(*pair.second));
            field("Set", list(set));
        } else if (dynamic_cast<P::Delete *>(&op)) {
            auto & del = const_cast<L::Delete &>(dynamic_cast<const L::Delete &>(logical));
            type("Delete");
            field("Relation Name", quote(tableName(db, del.getTable())));
            field("Branch", quote(branchName(db, del.getBranchId())));
        } else if (dynamic_cast<P::Print *>(&op) || dynamic_cast<P::TupleStream *>(&op)) {
            type("Result");
            std::vector<std::string> outputs;
            if (auto * result = dynamic_cast<const L::Result *>(&logical)) for (iu_p_t iu : result->selection) outputs.push_back(iuName(iu));
            field("Output", list(outputs));
        } else {
            type("Operator");
        }

        for (auto * child : op.getChildren()) node.children.push_back(describe(*child, db, plan));
        // the writes pass nothing on (their rows are those of their input)
        const bool write = dynamic_cast<P::Insert *>(&op) || dynamic_cast<P::Update *>(&op) || dynamic_cast<P::Delete *>(&op);
        if (active->analyze && !write) {
            if (op.getChildren().empty() || !(dynamic_cast<P::Print *>(&op) || dynamic_cast<P::TupleStream *>(&op))) {
                plan.counters.push_back(std::make_unique<uint64_t>(0));
                op.setRowCounter(plan.counters.back().get());
                node.rows = plan.counters.back().get();
            } else {
                node.rows = node.children.front().rows; // the result passes on what its input produces
            }
        }
        return node;
    }

    /// Before the code of a query is generated: its plan, and its row counters with ANALYZE
    void collectScans(Algebra::Physical::Operator & op, Database & db, std::set<std::string> & tables) {
        if (dynamic_cast<Algebra::Physical::TableScan *>(&op)) {
            auto & scan = const_cast<Algebra::Logical::TableScan &>(dynamic_cast<const Algebra::Logical::TableScan &>(op.getLogicalOperator()));
            const std::string name = tableName(db, scan.getTable());
            for (iu_p_t iu : scan.getProduced()) if (iu->iuType == InformationUnit::Type::ColumnRef) scanTables[iu->scanUID] = name;
            tables.insert(name + "#" + std::to_string(reinterpret_cast<uintptr_t>(&op)));
        }
        for (auto * child : op.getChildren()) collectScans(*child, db, tables);
    }

    void record(const std::string & query, Algebra::Physical::Operator & root, Database & db) {
        scanTables.clear();
        std::set<std::string> scans;
        collectScans(root, db, scans);
        qualify = scans.size() > 1;
        Plan plan;
        plan.query = query;
        plan.root = describe(root, db, plan);
        active->plans.push_back(std::move(plan));
    }

    void render(const Node & node, const std::string & indent, std::vector<std::pair<std::string, std::string>> extra,
                uint64_t rows, bool analyze, std::string & out, const std::vector<const Node *> * repeats = nullptr) {
        out += "{\n";
        std::vector<std::pair<std::string, std::string>> fields = std::move(extra);
        fields.insert(fields.end(), node.fields.begin(), node.fields.end());
        if (analyze && node.rows != nullptr) fields.emplace_back("Actual Rows", std::to_string(rows));
        for (size_t i = 0; i < fields.size(); ++i) {
            out += indent + "  " + quote(fields[i].first) + ": " + fields[i].second;
            out += (i + 1 < fields.size() || !node.children.empty()) ? ",\n" : "\n";
        }
        if (!node.children.empty()) {
            out += indent + "  \"Plans\": [\n";
            for (size_t c = 0; c < node.children.size(); ++c) {
                uint64_t childRows = 0;
                std::vector<const Node *> childRepeats;
                if (repeats != nullptr) for (auto * r : *repeats) childRepeats.push_back(&r->children[c]);
                for (auto * r : childRepeats) if (r->rows) childRows += *r->rows;
                if (repeats == nullptr && node.children[c].rows) childRows = *node.children[c].rows;
                out += indent + "    ";
                render(node.children[c], indent + "    ", {}, childRows, analyze, out, repeats ? &childRepeats : nullptr);
                out += (c + 1 < node.children.size()) ? ",\n" : "\n";
            }
            out += indent + "  ]\n";
        }
        out += indent + "}";
    }

    /// \returns The node without its row counts, to recognise the same plan run again
    std::string shape(const Node & node) {
        std::string out;
        for (auto & f : node.fields) out += f.first + "=" + f.second + ";";
        out += "[";
        for (auto & c : node.children) out += shape(c) + ",";
        return out + "]";
    }

    /// \returns The JSON of EXPLAIN: one plan, or of several queries the plan of each (the same query
    /// run again, as the steps of a recursive common table expression, once with its executions)
    std::string document(const State & state, double planning, double translation, double compilation, double execution) {
        std::string plan;
        const std::string indent = "    ";
        if (state.plans.size() == 1) {
            const Node & root = state.plans.front().root;
            render(root, indent, {}, root.rows ? *root.rows : 0, state.analyze, plan);
        } else {
            // runs of the same query with the same plan
            std::vector<std::vector<const Plan *>> groups;
            std::vector<std::string> shapes;
            for (auto & p : state.plans) {
                std::string s = p.query + "|" + shape(p.root);
                if (!groups.empty() && shapes.back() == s) groups.back().push_back(&p);
                else { groups.push_back({&p}); shapes.push_back(s); }
            }
            plan = "{\n" + indent + "  \"Node Type\": \"Queries\",\n" + indent + "  \"Plans\": [\n";
            for (size_t g = 0; g < groups.size(); ++g) {
                std::vector<const Node *> repeats;
                uint64_t rows = 0;
                for (auto * p : groups[g]) { repeats.push_back(&p->root); if (p->root.rows) rows += *p->root.rows; }
                std::vector<std::pair<std::string, std::string>> extra = {{"Query", quote(groups[g].front()->query)}};
                if (groups[g].size() > 1) extra.emplace_back("Executions", std::to_string(groups[g].size()));
                plan += indent + "    ";
                render(groups[g].front()->root, indent + "    ", extra, rows, state.analyze, plan, &repeats);
                plan += (g + 1 < groups.size()) ? ",\n" : "\n";
            }
            plan += indent + "  ]\n" + indent + "}";
        }
        std::string out = "[\n  {\n    \"Plan\": " + plan + ",\n";
        out += "    \"Planning Time\": " + number(planning) + ",\n";
        out += "    \"Translation Time\": " + number(translation);
        if (state.analyze) {
            out += ",\n    \"Compilation Time\": " + number(compilation) + ",\n";
            out += "    \"Execution Time\": " + number(execution);
        }
        return out + "\n  }\n]";
    }

    /// EXPLAIN [ ANALYZE ] [ VERBOSE ] [ ( option [ , ... ] ) ] statement
    /// \returns Whether the statement is EXPLAIN; then \p analyze and the explained \p statement
    bool parse(const std::string & query, bool & analyze, std::string & statement) {
        size_t i = 0;
        auto skip = [&]() { while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i; };
        auto word = [&]() {
            skip();
            size_t start = i;
            while (i < query.size() && (std::isalnum(static_cast<unsigned char>(query[i])) || query[i] == '_')) ++i;
            std::string w = query.substr(start, i - start);
            std::transform(w.begin(), w.end(), w.begin(), ::tolower);
            return w;
        };
        if (word() != "explain") return false;
        analyze = false;
        for (;;) {
            const size_t before = i;
            skip();
            if (i < query.size() && query[i] == '(') {
                ++i;
                for (;;) {
                    const std::string option = word();
                    skip();
                    std::string value;
                    if (i < query.size() && query[i] != ',' && query[i] != ')') value = word();
                    skip();
                    if (option == "analyze") {
                        if (value.empty() || value == "true" || value == "on" || value == "1") analyze = true;
                        else if (value == "false" || value == "off" || value == "0") analyze = false;
                        else throw tardisParser::syntactical_error("invalid value for ANALYZE: " + value);
                    } else if (option == "format") {
                        if (value != "json") throw tardisParser::syntactical_error("EXPLAIN supports FORMAT JSON only");
                    } else if (option != "verbose" && option != "costs" && option != "timing" && option != "summary") {
                        throw tardisParser::syntactical_error("unrecognized EXPLAIN option \"" + option + "\"");
                    }
                    if (i < query.size() && query[i] == ',') { ++i; continue; }
                    if (i < query.size() && query[i] == ')') { ++i; break; }
                    throw tardisParser::syntactical_error("expected ',' or ')' in the options of EXPLAIN");
                }
                continue;
            }
            const size_t wordStart = i;
            const std::string w = word();
            if (w == "analyze") { analyze = true; continue; }
            if (w == "verbose") continue;
            i = (w.empty()) ? before : wordStart;
            break;
        }
        statement = query.substr(i);
        if (statement.find_first_not_of(" \t\r\n;") == std::string::npos) throw tardisParser::syntactical_error("EXPLAIN needs a statement");
        return true;
    }

    /// A statement that EXPLAIN without ANALYZE cannot show, as it runs several queries
    void requireAnalyze(const char * what) {
        if (active != nullptr && !active->analyze) {
            throw semanticalAnalysis::semantic_sql_error(std::string("EXPLAIN of ") + what + " runs its queries: use EXPLAIN ANALYZE");
        }
    }

    } // end namespace explain

    static llvm::Function *compileQuery(const std::string &query, std::unique_ptr<Operator> &queryTree, QueryContext &queryContext) {
        timing::Phase phase(timing::current.translation);
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
            if (explain::active != nullptr) explain::record(query, *physicalTree, queryContext.analyzingContext.db);
            physicalTree->produce();

            queryFunc = funcGen.getFunction();
        }

        return queryFunc;
    }

    void printFunction(Native::Sql::SqlTuple *tuple) {
        std::cout << Native::Sql::toString(*tuple) << "\n";
    }

    static void compileAndExecuteStatement(const std::string &query, Database &db, void *callbackFunction);

    static StatementResult executeExplain(const std::string & statement, bool analyze, Database & db, const std::string & branch,
                                          const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                          const std::function<void(const StatementResult &)> & described,
                                          const Transaction * transaction);

    namespace settings {
        thread_local bool hashJoin = true, mergeJoin = true;

        static void apply() {
            Algebra::joinMethod() = (!hashJoin && mergeJoin) ? Algebra::JoinMethod::Merge : Algebra::JoinMethod::Hash;
        }
    }

    bool setSetting(const std::string & name, const std::string & value) {
        std::string n, v;
        std::transform(name.begin(), name.end(), std::back_inserter(n), ::tolower);
        std::transform(value.begin(), value.end(), std::back_inserter(v), ::tolower);
        if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') v = v.substr(1, v.size() - 2);
        if (n != "enable_hashjoin" && n != "enable_mergejoin") return false;
        bool on;
        if (v == "on" || v == "true" || v == "1" || v == "default") on = true;
        else if (v == "off" || v == "false" || v == "0") on = false;
        else throw std::invalid_argument("parameter \"" + n + "\" requires a Boolean value");
        (n == "enable_hashjoin" ? settings::hashJoin : settings::mergeJoin) = on;
        settings::apply();
        return true;
    }

    std::string getSetting(const std::string & name) {
        std::string n;
        std::transform(name.begin(), name.end(), std::back_inserter(n), ::tolower);
        if (n == "enable_hashjoin") return settings::hashJoin ? "on" : "off";
        if (n == "enable_mergejoin") return settings::mergeJoin ? "on" : "off";
        return "";
    }

    Timing compileAndExecute(const std::string &query, Database &db, void *callbackFunction) {
        timing::Statement clock;
        {
            // SET enable_hashjoin | enable_mergejoin = on | off (pgserver handles SET itself)
            static const std::regex set(R"(^\s*set\s+(?:session\s+)?(enable_hashjoin|enable_mergejoin)\s*(?:=|\s+to\s+)\s*('?\w+'?)\s*;?\s*$)", std::regex::icase);
            std::smatch match;
            if (std::regex_match(query, match, set)) {
                setSetting(match[1].str(), match[2].str());
                return clock.finish();
            }
        }
        bool analyze = false;
        std::string explained;
        if (explain::parse(query, analyze, explained)) {
            // the plan as it is, not as a tuple
            auto print = [callbackFunction](Native::Sql::SqlTuple * tuple) {
                if (callbackFunction != nullptr) reinterpret_cast<void (*)(Native::Sql::SqlTuple *)>(callbackFunction)(tuple);
                else std::cout << Native::Sql::toString(*tuple->values[0]) << "\n";
            };
            executeExplain(explained, analyze, db, "master", print, nullptr, nullptr);
            return clock.finish();
        }
        try {
            compileAndExecuteStatement(query, db, callbackFunction);
        } catch (const semanticalAnalysis::general_aggregation_required &) {
            auto print = (callbackFunction != nullptr) ? reinterpret_cast<void (*)(Native::Sql::SqlTuple *)>(callbackFunction) : &printFunction;
            execute(query, db, "master", [print](Native::Sql::SqlTuple * tuple) { print(tuple); });
        }
        return clock.finish();
    }

    namespace tablefunctions { static bool mentioned(const std::string & lower); }

    static void compileAndExecuteStatement(const std::string &query, Database &db, void *callbackFunction) {
        {
            // WITH, UNION, VALUES, table functions and SELECT without FROM run as the statements of a session
            std::string lower;
            std::transform(query.begin(), query.end(), std::back_inserter(lower), ::tolower);
            size_t first = lower.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && (lower.compare(first, 4, "with") == 0 || lower.compare(first, 6, "values") == 0 ||
                (lower.compare(first, 6, "select") == 0 && tablefunctions::mentioned(lower)) ||
                lower.compare(first, 1, "(") == 0 || (lower.compare(first, 6, "select") == 0 &&
                (lower.find("union") != std::string::npos || lower.find("intersect") != std::string::npos || lower.find("order") != std::string::npos ||
                 lower.find("except") != std::string::npos || lower.find(" from ") == std::string::npos)))) {
                auto print = (callbackFunction != nullptr) ? reinterpret_cast<void (*)(Native::Sql::SqlTuple *)>(callbackFunction) : &printFunction;
                execute(query, db, "master", [print](Native::Sql::SqlTuple * tuple) { print(tuple); });
                return;
            }
        }
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

        {
            timing::Phase phase(timing::current.parsing);
            tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
            QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.parsingContext);
        }
        StatementLatches latches(db, queryContext.analyzingContext.parserResult);

        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        {
            timing::Phase phase(timing::current.analysis);
            analyser->verify();
            analyser->constructTree();
        }
        auto &queryTree = queryContext.analyzingContext.joinedTree;
        if (queryTree == nullptr) return;
        latches.latchReadBranches(db, queryContext.analyzingContext.branchIds);
        if (callbackFunction == nullptr) callbackFunction = (void*)&printFunction;
        // COPY ... TO writes the tuples into its file (an exception must not leave the generated code)
        auto & copyWriter = queryContext.analyzingContext.copyWriter;
        std::exception_ptr failure;
        if (copyWriter) {
            queryContext.tupleSink = [&copyWriter, &failure](Native::Sql::SqlTuple * tuple) {
                if (failure) return;
                try {
                    copyWriter->write(*tuple);
                } catch (...) {
                    failure = std::current_exception();
                }
            };
        }

        auto queryFunc = compileQuery(query, queryTree,queryContext);
        if (queryFunc == nullptr) return;

        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds,queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::executeFunction(queryFunc, args, callbackFunction);
        if (failure) std::rethrow_exception(failure);
        if (copyWriter) copyWriter->finish();
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
        sqlExpressions::node_p_t having; // over the table of the groups

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
            // HAVING: its aggregates join those of the select list, its columns must be grouped
            if (select.having) having = std::make_shared<Node>(rewrite(*select.having));
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
    /// into a temporary table, and the select list over it (another query), whose WHERE is the condition of HAVING
    static StatementResult runAggregation(const semanticalAnalysis::SelectStatement & select, Database & db, const std::string & branch,
                                          const Transaction * transaction, TemporaryTables & temporaries,
                                          const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                          const std::function<void(const StatementResult &)> & described, bool describeOnly) {
        using namespace aggregation;
        using Native::Sql::nullLiteral;
        explain::requireAnalyze("an aggregate computed from several queries");
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
        if (plan.having) {
            semanticalAnalysis::Predicate predicate;
            predicate.op = "expr";
            predicate.expression = plan.having;
            output->predicates.push_back(std::move(predicate));
        }
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
            timing::Phase phase(timing::current.analysis);
            analyser->verify();
        } catch (const semanticalAnalysis::general_aggregation_required &) {
            moduleGen.reset(); // the queries of the aggregation generate their own modules
            return runAggregation(*parserResult.selectStmt, db, branch, transaction, temporaries, sink, described, describeOnly);
        }
        {
            timing::Phase phase(timing::current.analysis);
            analyser->constructTree();
        }
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
        {
            timing::Phase phase(timing::current.parsing);
            tardisParser::SQLParser::parseStatement(queryContext.parsingContext, text + ";");
        }
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
        explain::requireAnalyze("WITH, VALUES and set operations");
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
                {
                    timing::Phase phase(timing::current.parsing);
                    tardisParser::SQLParser::parseStatement(context.parsingContext, term->term + ";");
                }
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
        {
            timing::Phase phase(timing::current.parsing);
            tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
            QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult, queryContext.parsingContext);
        }
        if (explain::active != nullptr) {
            // the other statements run in their analysis, which EXPLAIN must not do
            using OpType = semanticalAnalysis::SQLParserResult::OpType;
            const auto op = queryContext.analyzingContext.parserResult.opType;
            if (op != OpType::Select && op != OpType::Insert && op != OpType::Update && op != OpType::Delete) {
                throw semanticalAnalysis::semantic_sql_error("EXPLAIN takes SELECT, INSERT, UPDATE, DELETE, WITH, VALUES and set operations");
            }
        }
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
        {
            timing::Phase phase(timing::current.analysis);
            analyser->verify();
            analyser->constructTree();
        }
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
        // COPY ... TO writes the tuples into its file: the client receives only their number
        auto & copyWriter = queryContext.analyzingContext.copyWriter;
        if (copyWriter) {
            result.returnsRows = false;
            result.columnNames.clear();
            result.columnTypes.clear();
        }

        if (described) described(result);

        auto queryFunc = compileQuery(query, queryTree, queryContext);
        if (queryFunc == nullptr) return result;
        if (explain::active != nullptr && !explain::active->analyze) return result; // EXPLAIN: the plan only
        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds, queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;
        uint64_t rows = 0;
        std::exception_ptr failure;
        const bool counting = result.returnsRows || copyWriter;
        std::function<void(Native::Sql::SqlTuple *)> target = sink;
        if (copyWriter) target = [&copyWriter](Native::Sql::SqlTuple * tuple) { copyWriter->write(*tuple); };
        if (counting) {
            // count the tuples on their way to the sink; an exception of the sink must not leave the result
            // latch of a parallel scan, so the first one ends the statement after the scan
            queryContext.tupleSink = [&rows, &target, &failure](Native::Sql::SqlTuple * tuple) {
                if (failure) return;
                try {
                    ++rows;
                    target(tuple);
                } catch (...) {
                    failure = std::current_exception();
                }
            };
        }
        QueryExecutor::executeFunction(queryFunc, args, (void *) &printFunction);
        if (failure) std::rethrow_exception(failure);
        if (copyWriter) copyWriter->finish();
        result.rows = counting ? rows : queryContext.executionContext.affectedRows;
        return result;
    }

    /// ORDER BY of a query (SELECT, WITH, VALUES, set operations): the statement runs without it, and its
    /// rows are sorted before they reach the sink. A key is a position in the select list, the name of a
    /// column of the result, or (of a single SELECT) any expression, which the statement computes as a
    /// hidden column at the end of its select list; ASC or DESC, NULLS FIRST or LAST (NULLs are greater than
    /// all values by default, as in PostgreSQL). Strings sort by their bytes (collation C).
    namespace order {

    using with::Lexeme;

    struct Key {
        size_t position = 0;  // 1 ... the columns of the result; 0 for a name or a hidden column
        std::string name;     // a column of the result by its name
        size_t hidden = SIZE_MAX; // the hidden column (counted from the first)
        bool descending = false;
        bool nullsFirst = false;
        size_t column = 0;    // resolved: of the rows
    };

    struct OrderBy {
        std::string query;    // without ORDER BY, with the hidden columns
        std::vector<Key> keys;
        size_t hidden = 0;    // the hidden columns at the end of each row
    };

    /// \returns Whether the lexemes are one name (a word, in lower case, or a quoted identifier)
    static bool isName(const std::vector<Lexeme> & lexemes) {
        return lexemes.size() == 1 && (lexemes[0].kind == Lexeme::Word || lexemes[0].kind == Lexeme::Quoted);
    }

    /// \returns Whether \p sql is a query with ORDER BY; then \p order
    static bool parse(const std::string & sql, OrderBy & order) {
        size_t first = sql.find_first_not_of(" \t\r\n(");
        if (first == std::string::npos) return false;
        std::string head;
        for (size_t i = first; i < sql.size() && std::isalpha(static_cast<unsigned char>(sql[i])); ++i) {
            head += static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i])));
        }
        if (head != "select" && head != "with" && head != "values") return false;
        {
            std::string lower;
            std::transform(sql.begin(), sql.end(), std::back_inserter(lower), ::tolower);
            if (lower.find("order") == std::string::npos) return false;
        }
        auto lexemes = with::lex(sql);
        while (!lexemes.empty() && lexemes.back().kind == Lexeme::Semicolon) lexemes.pop_back();
        const size_t n = lexemes.size();

        // the ORDER BY outside parentheses, and what the query is
        size_t orderAt = SIZE_MAX, lastSelect = SIZE_MAX, depth = 0;
        bool setQuery = false;
        for (size_t i = 0; i < n; ++i) {
            const Lexeme & l = lexemes[i];
            if (l.kind == Lexeme::Open) { ++depth; continue; }
            if (l.kind == Lexeme::Close) { if (depth > 0) --depth; continue; }
            if (depth > 0 || l.kind != Lexeme::Word) continue;
            if (with::isWord(lexemes, i, "order") && with::isWord(lexemes, i + 1, "by")) {
                if (orderAt != SIZE_MAX) throw with::syntaxError("a query has one ORDER BY");
                orderAt = i;
            } else if (orderAt == SIZE_MAX) {
                if (l.word == "select") lastSelect = i;
                else if (with::isSetOperator(l)) setQuery = true;
            }
        }
        if (orderAt == SIZE_MAX) return false;
        if (orderAt + 2 >= n) throw with::syntaxError("Expected the keys of ORDER BY");
        // a query in parentheses before ORDER BY ( (SELECT ...) ORDER BY 1 ) is one of a set query
        if (lexemes[0].kind == Lexeme::Open) setQuery = true;
        const bool single = !setQuery && lastSelect != SIZE_MAX; // VALUES without SELECT is a set query

        // the aliases of the select list of a single SELECT: a name that is one of them is the result's column
        std::set<std::string> aliases;
        bool star = false;        // SELECT *, which takes no further columns: names are of the result
        size_t listEnd = orderAt; // the end of the select list
        if (single) {
            depth = 0;
            for (size_t i = lastSelect + 1; i < orderAt; ++i) {
                if (lexemes[i].kind == Lexeme::Open) ++depth;
                else if (lexemes[i].kind == Lexeme::Close) --depth;
                else if (depth == 0 && (with::isWord(lexemes, i, "from") || with::isWord(lexemes, i, "where") ||
                                        with::isWord(lexemes, i, "group") || with::isWord(lexemes, i, "having"))) {
                    listEnd = i;
                    break;
                }
            }
            for (size_t i = lastSelect + 1; i < listEnd; ++i) {
                if (lexemes[i].kind == Lexeme::Other && sql[lexemes[i].begin] == '*' &&
                    (i == lastSelect + 1 || lexemes[i - 1].kind == Lexeme::Comma)) star = true;
            }
            depth = 0;
            for (size_t i = lastSelect + 1; i < listEnd; ++i) {
                if (lexemes[i].kind == Lexeme::Open) ++depth;
                else if (lexemes[i].kind == Lexeme::Close) --depth;
                const bool itemEnd = depth == 0 && (i + 1 == listEnd || lexemes[i + 1].kind == Lexeme::Comma);
                if (!itemEnd || i == lastSelect + 1) continue;
                const Lexeme & last = lexemes[i], & before = lexemes[i - 1];
                if (last.kind != Lexeme::Word && last.kind != Lexeme::Quoted) continue;
                // expr AS name, or expr name after a word, a parenthesis or a quoted name (not a.b)
                if (with::isWord(lexemes, i - 1, "as") || (before.kind == Lexeme::Word && before.word != "select" &&
                    before.word != "distinct") || before.kind == Lexeme::Close || before.kind == Lexeme::Quoted ||
                    before.kind == Lexeme::String) {
                    aliases.insert(last.word);
                }
            }
        }

        std::string hiddenColumns;
        for (const std::string & item : with::splitItems(sql, lexemes, orderAt + 2, n)) {
            auto keyLexemes = with::lex(item);
            Key key;
            size_t end = keyLexemes.size();
            // [ ASC | DESC ] [ NULLS { FIRST | LAST } ]
            bool nullsGiven = false;
            if (end >= 2 && with::isWord(keyLexemes, end - 2, "nulls") &&
                (with::isWord(keyLexemes, end - 1, "first") || with::isWord(keyLexemes, end - 1, "last"))) {
                key.nullsFirst = keyLexemes[end - 1].word == "first";
                nullsGiven = true;
                end -= 2;
            }
            if (end >= 1 && (with::isWord(keyLexemes, end - 1, "asc") || with::isWord(keyLexemes, end - 1, "desc"))) {
                key.descending = keyLexemes[end - 1].word == "desc";
                --end;
            }
            if (!nullsGiven) key.nullsFirst = key.descending;
            if (end == 0) throw with::syntaxError("Expected an expression in ORDER BY");
            keyLexemes.resize(end);
            const std::string expression = item.substr(0, keyLexemes.back().end);
            if (std::all_of(expression.begin(), expression.end(), ::isdigit)) {
                key.position = std::stoul(expression);
                if (key.position == 0) throw semanticalAnalysis::semantic_sql_error("ORDER BY position 0 is not in select list");
            } else if (isName(keyLexemes) && (!single || star || aliases.count(keyLexemes[0].word) > 0)) {
                key.name = keyLexemes[0].word; // a column of the result
            } else if (single && star) {
                throw semanticalAnalysis::semantic_sql_error("ORDER BY of SELECT * takes the positions or names of the result's columns, not '" + expression + "'");
            } else if (single) {
                key.hidden = order.hidden++;
                hiddenColumns += ", " + expression;
            } else {
                throw semanticalAnalysis::semantic_sql_error("ORDER BY of UNION, INTERSECT, EXCEPT or VALUES takes the positions or names of the result's columns, not '" + expression + "'");
            }
            order.keys.push_back(std::move(key));
        }

        // the hidden columns after the last item of the select list
        const size_t queryEnd = lexemes[orderAt - 1].end;
        if (listEnd < orderAt) {
            order.query = sql.substr(0, lexemes[listEnd - 1].end) + hiddenColumns + " " +
                          sql.substr(lexemes[listEnd].begin, queryEnd - lexemes[listEnd].begin);
        } else {
            order.query = sql.substr(0, queryEnd) + hiddenColumns;
        }
        order.query += ";";
        return true;
    }

    /// A copy of a value of a tuple of the sink, which stays valid after the query; a varchar or char points
    /// to its characters, which \p strings keeps
    static Native::Sql::value_op_t copy(const Native::Sql::Value & value, std::deque<std::string> & strings) {
        if (Native::Sql::isNullable(value) && static_cast<const Native::Sql::NullableValue &>(value).isNull()) {
            return Native::Sql::NullableValue::getNull(value.type);
        }
        const Native::Sql::Value & plain = Native::Sql::getAssociatedValue(value);
        strings.push_back(Native::Sql::toString(plain));
        return Native::Sql::toColumnValue(Native::Sql::Value::castString(strings.back(), plain.type), value.type);
    }

    } // end namespace order

    static StatementResult executeAny(const std::string & query, Database & db, const std::string & branch,
                                      const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                      const std::function<void(const StatementResult &)> & described,
                                      const Transaction * transaction);

    /// Calls of table functions (TableFunctions.hpp) in FROM and in the select list of a query without FROM
    namespace tablefunctions {

    using with::Lexeme;
    using semanticalAnalysis::semantic_sql_error;

    struct Edit {
        size_t begin, end; // replaced in the statement
        std::string text;
    };

    struct Call {
        const TableFunctions::TableFunction * function;
        std::vector<std::string> arguments;
        std::string alias;                // of the table, if any
        std::vector<std::string> columns; // the names of the columns, if given
        std::string table;                // the temporary table
        bool selectList = false;          // called in the select list
    };

    static bool isWordIn(const Lexeme & lexeme, std::initializer_list<const char *> words) {
        if (lexeme.kind != Lexeme::Word) return false;
        for (const char * word : words) if (lexeme.word == word) return true;
        return false;
    }

    /// The words that begin a clause (a FROM item and a select list end before them)
    static bool endsList(const Lexeme & lexeme) {
        return isWordIn(lexeme, {"where", "group", "having", "order", "limit", "offset", "union", "intersect", "except", "window", "fetch", "for"});
    }

    /// \returns Whether the statement (in lower case) may call a table function
    static bool mentioned(const std::string & lower) {
        for (const std::string & name : TableFunctions::names()) if (lower.find(name) != std::string::npos) return true;
        return false;
    }

    /// \returns The statement with each call of a table function replaced by a temporary table, listed in \p calls
    static std::string rewrite(const std::string & sql, std::vector<Call> & calls) {
        static std::atomic<uint64_t> counter{0};
        const std::vector<Lexeme> lexemes = with::lex(sql);
        const size_t n = lexemes.size();
        std::vector<Edit> edits;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (lexemes[i].kind != Lexeme::Word || lexemes[i + 1].kind != Lexeme::Open) continue;
            const TableFunctions::TableFunction * function = TableFunctions::find(lexemes[i].word);
            if (function == nullptr) continue;
            const std::string name = function->name();
            const size_t close = with::matching(lexemes, i + 1, n);
            Call call;
            call.function = function;
            call.arguments = (close > i + 2) ? with::splitItems(sql, lexemes, i + 2, close) : std::vector<std::string>();
            call.table = "tardisdb_function_" + std::to_string(++counter);

            // the clause of the call: the nearest clause word before it in its parentheses
            std::string clause;
            for (size_t k = i, depth = 0; k-- > 0;) {
                if (lexemes[k].kind == Lexeme::Close) { ++depth; continue; }
                if (lexemes[k].kind == Lexeme::Open) { if (depth-- == 0) break; continue; }
                if (depth == 0 && (isWordIn(lexemes[k], {"select", "from", "join"}) || endsList(lexemes[k]) || isWordIn(lexemes[k], {"on", "set", "values"}))) {
                    clause = lexemes[k].word;
                    break;
                }
            }
            if (clause == "from" || clause == "join") {
                // [AS] alias [( column, ... )]
                size_t k = close + 1;
                const bool as = isWord(lexemes, k, "as");
                if (as) ++k;
                if (k < n && (lexemes[k].kind == Lexeme::Quoted || (lexemes[k].kind == Lexeme::Word && !endsList(lexemes[k]) &&
                        !isWordIn(lexemes[k], {"join", "inner", "left", "right", "full", "cross", "natural", "on", "using", "version", "lateral"})))) {
                    call.alias = sql.substr(lexemes[k].begin, lexemes[k].end - lexemes[k].begin);
                    ++k;
                } else if (as) {
                    throw with::syntaxError("Expected an alias of " + name + " after AS");
                }
                if (!call.alias.empty() && k < n && lexemes[k].kind == Lexeme::Open) {
                    const size_t end = with::matching(lexemes, k, n);
                    call.columns = with::splitItems(sql, lexemes, k + 1, end);
                    k = end + 1;
                }
                const size_t end = (k > close + 1) ? lexemes[k - 1].end : lexemes[close].end;
                edits.push_back({lexemes[i].begin, end, call.table + " " + (call.alias.empty() ? name : call.alias)});
            } else if (clause == "select") {
                // the select list of a query without FROM: the call becomes the column, the table its FROM
                size_t k = close + 1, depth = 0;
                for (; k < n; ++k) {
                    if (lexemes[k].kind == Lexeme::Open) { ++depth; continue; }
                    if (lexemes[k].kind == Lexeme::Close) { if (depth-- == 0) break; continue; }
                    if (depth > 0) continue;
                    if (lexemes[k].kind == Lexeme::Semicolon || endsList(lexemes[k])) break;
                    if (isWord(lexemes, k, "from")) {
                        throw semantic_sql_error(name + " in the select list of a query with FROM: call it in FROM instead");
                    }
                    if (lexemes[k].kind == Lexeme::Word && k + 1 < n && lexemes[k + 1].kind == Lexeme::Open && TableFunctions::find(lexemes[k].word)) {
                        throw semantic_sql_error("one table function per select list");
                    }
                }
                call.alias = name;
                call.selectList = true;
                edits.push_back({lexemes[i].begin, lexemes[close].end, name});
                const size_t at = (k < n) ? lexemes[k].begin : sql.size();
                edits.push_back({at, at, " FROM " + call.table + " " + name + " "});
            } else {
                throw semantic_sql_error(name + " returns a table: call it in FROM or in a select list");
            }
            calls.push_back(std::move(call));
            i = close; // calls in the arguments are evaluated with them
        }
        if (edits.empty()) return sql;
        std::sort(edits.begin(), edits.end(), [](const Edit & a, const Edit & b) { return a.begin < b.begin || (a.begin == b.begin && a.end < b.end); });
        std::string out;
        size_t at = 0;
        for (const Edit & edit : edits) {
            if (edit.begin < at) throw semantic_sql_error("a table function cannot be rewritten here");
            out += sql.substr(at, edit.begin - at) + edit.text;
            at = edit.end;
        }
        return out + sql.substr(at);
    }

    /// Evaluates the arguments of a call (constant expressions; NULL literals stay unevaluated)
    static std::vector<TableFunctions::Argument> evaluate(const Call & call, Database & db, const std::string & branch, const Transaction * transaction) {
        std::vector<TableFunctions::Argument> arguments(call.arguments.size());
        std::vector<size_t> evaluated;
        std::string query = "SELECT ";
        for (size_t a = 0; a < call.arguments.size(); ++a) {
            arguments[a].text = call.arguments[a];
            std::string lower;
            std::transform(call.arguments[a].begin(), call.arguments[a].end(), std::back_inserter(lower), ::tolower);
            if (lower == "null") {
                arguments[a].null = true;
                continue;
            }
            query += std::string(evaluated.empty() ? "" : ", ") + call.arguments[a];
            evaluated.push_back(a);
        }
        if (evaluated.empty()) return arguments;
        explain::State * explaining = explain::active; // the arguments are no part of the plan
        explain::active = nullptr;
        struct Restore { explain::State * state; ~Restore() { explain::active = state; } } restore{explaining};
        try {
            executeAny(query + ";", db, branch, [&](Native::Sql::SqlTuple * tuple) {
                for (size_t v = 0; v < tuple->values.size() && v < evaluated.size(); ++v) {
                    auto & value = tuple->values[v];
                    TableFunctions::Argument & argument = arguments[evaluated[v]];
                    if (Native::Sql::isNullable(*value) && static_cast<const Native::Sql::NullableValue &>(*value).isNull()) argument.null = true;
                    else argument.value = Native::Sql::toString(Native::Sql::getAssociatedValue(*value));
                }
            }, [&](const StatementResult & result) {
                for (size_t v = 0; v < result.columnTypes.size() && v < evaluated.size(); ++v) arguments[evaluated[v]].type = result.columnTypes[v];
            }, transaction);
        } catch (const std::exception & e) {
            throw semantic_sql_error("the arguments of " + call.function->name() + " are constants: " + e.what());
        }
        return arguments;
    }

    /// Computes the rows of a call into a new temporary table, named by the call
    static void materialize(const Call & call, Database & db, const std::string & branch, const Transaction * transaction,
                            TemporaryTables & temporaries) {
        std::vector<TableFunctions::Argument> arguments = evaluate(call, db, branch, transaction);
        std::vector<TableFunctions::Column> columns = call.function->columns(arguments);
        if (call.selectList && columns.size() != 1) throw semantic_sql_error(call.function->name() + " returns several columns: call it in FROM");
        // the names: the alias names the column of a function of one column, a list of the alias the first columns
        if (columns.size() == 1 && !call.alias.empty()) columns.front().name = call.alias;
        if (call.columns.size() > columns.size()) {
            throw semantic_sql_error(call.function->name() + " returns " + std::to_string(columns.size()) + " columns, not " + std::to_string(call.columns.size()));
        }
        for (size_t c = 0; c < call.columns.size(); ++c) columns[c].name = call.columns[c];

        // the table, under the latch that keeps the branches of the catalog as they are
        std::unique_lock<std::mutex> serial;
        std::shared_lock<SharedLatch> catalog;
        if (latching == Latching::Serial) serial = std::unique_lock<std::mutex>(serialLatch);
        else catalog = std::shared_lock<SharedLatch>(db._catalogLatch);
        ModuleGen moduleGen("TemporaryTable"); // the sizes of the columns take the data layout
        std::vector<std::pair<std::string, Sql::SqlType>> schema;
        for (const auto & column : columns) schema.emplace_back(column.name, column.type);
        Table & table = temporaries.create(schema);
        temporaries.bind(call.table, &table);
        QueryContext context(db);
        context.executionContext.branchId = master_branch_id;
        call.function->rows(arguments, columns, [&](std::vector<Native::Sql::value_op_t> values) {
            Native::Sql::SqlTuple tuple(std::move(values));
            insert_tuple(tuple, table, context);
        });
    }

    } // end namespace tablefunctions

    static StatementResult executeOrdered(const order::OrderBy & order, Database & db, const std::string & branch,
                                          const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                          const std::function<void(const StatementResult &)> & described,
                                          const Transaction * transaction);

    static StatementResult executeAny(const std::string & query, Database & db, const std::string & branch,
                                      const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                      const std::function<void(const StatementResult &)> & described,
                                      const Transaction * transaction) {
        {
            // table functions: their rows first, in temporary tables that the rewritten statement reads
            std::vector<tablefunctions::Call> calls;
            std::string rewritten;
            {
                timing::Phase phase(timing::current.parsing);
                rewritten = tablefunctions::rewrite(query, calls);
            }
            if (!calls.empty()) {
                TemporaryTables temporaries(db);
                for (const tablefunctions::Call & call : calls) tablefunctions::materialize(call, db, branch, transaction, temporaries);
                return executeAny(rewritten, db, branch, sink, described, transaction);
            }
        }
        order::OrderBy order;
        bool ordered;
        with::WithStatement setStatement;
        bool isSet = false;
        {
            timing::Phase phase(timing::current.parsing);
            ordered = order::parse(query, order);
            if (!ordered) isSet = with::parseSetStatement(query, setStatement);
        }
        if (ordered) return executeOrdered(order, db, branch, sink, described, transaction);
        if (isSet) {
            return executeSetStatement(setStatement, db, branch, sink, described, transaction);
        }
        try {
            return executeStatement(query, db, branch, sink, described, transaction);
        } catch (const semanticalAnalysis::general_aggregation_required &) {
            explain::requireAnalyze("an aggregate computed from several queries");
            // the latches and the module of the statement are gone: its query runs as the queries of WITH
            with::WithStatement statement;
            statement.query = with::makeTerm(withoutEnd(query));
            return executeSetStatement(statement, db, branch, sink, described, transaction);
        }
    }

    static StatementResult executeOrdered(const order::OrderBy & order, Database & db, const std::string & branch,
                                          const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                          const std::function<void(const StatementResult &)> & described,
                                          const Transaction * transaction) {
        // the rows of the statement, copied (the sink may run in several threads at once)
        std::vector<std::vector<Native::Sql::value_op_t>> rows;
        std::deque<std::string> strings; // the characters of the copied varchar and char values
        std::mutex mutex;
        StatementResult result = executeAny(order.query, db, branch, [&](Native::Sql::SqlTuple * tuple) {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<Native::Sql::value_op_t> row;
            row.reserve(tuple->values.size());
            for (auto & value : tuple->values) row.push_back(order::copy(*value, strings));
            rows.push_back(std::move(row));
        }, nullptr, transaction);
        if (!result.returnsRows) throw semanticalAnalysis::semantic_sql_error("ORDER BY needs a query");

        // the keys as columns of the rows
        const size_t visible = result.columnNames.size() - order.hidden;
        std::vector<order::Key> keys = order.keys;
        for (auto & key : keys) {
            if (key.hidden != SIZE_MAX) {
                key.column = visible + key.hidden;
            } else if (key.position > 0) {
                if (key.position > visible) {
                    throw semanticalAnalysis::semantic_sql_error("ORDER BY position " + std::to_string(key.position) + " is not in select list");
                }
                key.column = key.position - 1;
            } else {
                size_t found = SIZE_MAX;
                for (size_t c = 0; c < visible; ++c) {
                    if (result.columnNames[c] != key.name) continue;
                    if (found != SIZE_MAX) throw semanticalAnalysis::semantic_sql_error("ORDER BY \"" + key.name + "\" is ambiguous");
                    found = c;
                }
                if (found == SIZE_MAX) throw semanticalAnalysis::semantic_sql_error("column \"" + key.name + "\" does not exist");
                key.column = found;
            }
            if (!Native::Sql::hasOrder(result.columnTypes[key.column])) {
                throw semanticalAnalysis::semantic_sql_error("ORDER BY of a column of type " + Sql::getName(result.columnTypes[key.column]) + " is not supported");
            }
        }
        auto isNull = [](const Native::Sql::Value & v) {
            return Native::Sql::isNullable(v) && static_cast<const Native::Sql::NullableValue &>(v).isNull();
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const auto & x, const auto & y) {
            for (auto & key : keys) {
                const Native::Sql::Value & a = *x[key.column], & b = *y[key.column];
                const bool aNull = isNull(a), bNull = isNull(b);
                if (aNull || bNull) {
                    if (aNull == bNull) continue;
                    return aNull == key.nullsFirst;
                }
                const int c = Native::Sql::compareValues(Native::Sql::getAssociatedValue(a), Native::Sql::getAssociatedValue(b));
                if (c != 0) return key.descending ? c > 0 : c < 0;
            }
            return false;
        });

        result.columnNames.resize(visible);
        result.columnTypes.resize(visible);
        if (described) described(result);
        for (auto & row : rows) {
            row.resize(visible);
            Native::Sql::SqlTuple tuple(std::move(row));
            sink(&tuple);
        }
        return result;
    }

    /// EXPLAIN [ANALYZE] of \p statement: one row, the plan in JSON
    static StatementResult executeExplain(const std::string & statement, bool analyze, Database & db, const std::string & branch,
                                          const std::function<void(Native::Sql::SqlTuple *)> & sink,
                                          const std::function<void(const StatementResult &)> & described,
                                          const Transaction * transaction) {
        if (explain::active != nullptr) throw tardisParser::syntactical_error("EXPLAIN cannot explain EXPLAIN");
        explain::State state;
        state.analyze = analyze;
        explain::active = &state;
        struct Reset { ~Reset() { explain::active = nullptr; } } reset;

        const Timing before = timing::current;
        const double llvmBefore = QueryExecutor::runTimes().compilation;
        const auto start = timing::Clock::now();
        executeAny(statement, db, branch, [](Native::Sql::SqlTuple *) { }, nullptr, transaction); // the rows are not shown
        const double elapsed = timing::since(start);
        const Timing & after = timing::current;
        const double planning = (after.parsing - before.parsing) + (after.analysis - before.analysis);
        const double translation = after.translation - before.translation;
        const double llvm = QueryExecutor::runTimes().compilation - llvmBefore;
        const std::string json = explain::document(state, planning, translation, llvm, std::max(0.0, elapsed - planning - translation - llvm));

        StatementResult result;
        result.tag = "EXPLAIN";
        result.columnNames = {"QUERY PLAN"};
        result.columnTypes = {Sql::getTextTy(false)};
        result.returnsRows = true;
        result.rows = 1;
        if (described) described(result);
        std::vector<Native::Sql::value_op_t> values;
        values.push_back(Native::Sql::castColumnValue(json, Sql::getTextTy(false)));
        Native::Sql::SqlTuple tuple(std::move(values));
        sink(&tuple);
        return result;
    }

    StatementResult execute(const std::string & query, Database & db, const std::string & branch,
                            const std::function<void(Native::Sql::SqlTuple *)> & sink,
                            const std::function<void(const StatementResult &)> & described,
                            const Transaction * transaction) {
        timing::Statement clock;
        bool analyze = false;
        std::string explained;
        StatementResult result = explain::parse(query, analyze, explained)
                ? executeExplain(explained, analyze, db, branch, sink, described, transaction)
                : executeAny(query, db, branch, sink, described, transaction);
        if (clock.outer) result.timing = clock.finish();
        return result;
    }

    BenchmarkResult compileAndBenchmark(const std::string &query, Database &db, void *callbackFunction) {
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

        const auto parsingStart = std::chrono::high_resolution_clock::now();
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.parsingContext);
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
