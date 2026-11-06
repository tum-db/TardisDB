// CREATE INDEX ... VERSION b, DROP BRANCH, MERGE BRANCH and ALTER TABLE ... VERSION b

#include "semanticAnalyser/SemanticAnalyser.hpp"

#include "foundations/version_management.hpp"

namespace semanticalAnalysis {

    static branch_id_t lookupBranch(Database &db, const std::string &name) {
        auto it = db._branchMapping.find(name);
        if (it == db._branchMapping.end()) throw semantic_sql_error("branch '" + name + "' does not exist");
        return it->second;
    }

    //
    // CREATE INDEX [ name ] ON t ( c ) [ VERSION b ]
    //

    void CreateIndexAnalyser::verify() {
        Database &db = _context.db;
        CreateIndexStatement *stmt = _context.parserResult.createIndexStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        Table *table = db.getTable(stmt->relation.name);
        if (table == nullptr) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        branch_id_t branch = lookupBranch(db, stmt->relation.version);
        if (stmt->columns.size() != 1) throw semantic_sql_error("indexes on several columns are not supported yet");
        const std::string &column = stmt->columns.front().name;
        ci_p_t ci = table->findColumn(column, branch);
        if (ci == nullptr) throw semantic_sql_error("column '" + column + "' does not exist");
        if (table->getIndex(ci->index, branch) != nullptr)
            throw semantic_sql_error("branch '" + stmt->relation.version + "' already has an index on " + stmt->relation.name + " (" + column + ")");
        for (auto &index : table->_indexes) {
            if (!stmt->indexName.empty() && index->name == stmt->indexName)
                throw semantic_sql_error("index '" + stmt->indexName + "' already exists");
        }
    }

    void CreateIndexAnalyser::constructTree() {
        Database &db = _context.db;
        CreateIndexStatement *stmt = _context.parserResult.createIndexStmt;
        Table *table = db.getTable(stmt->relation.name);
        branch_id_t branch = lookupBranch(db, stmt->relation.version);
        const std::string &column = stmt->columns.front().name;
        const size_t position = table->findColumn(column, branch)->index;

        std::string name = stmt->indexName;
        if (name.empty()) name = stmt->relation.name + "_" + column + "_" + db._branches.at(branch)->name;
        table->_indexes.push_back(std::make_unique<BranchIndex>(name, position, branch));
        build_index(*table, *table->_indexes.back());

        const size_t size = table->_indexes.back()->size();
        std::cout << "Created index " << name << " in branch " << db._branches.at(branch)->name << " ("
                  << size << " tuple" << (size != 1 ? "s" : "") << ")\n";
        _context.joinedTree = nullptr;
    }

    //
    // DROP BRANCH b [ CASCADE ]
    //

    void DropBranchAnalyser::verify() {
        Database &db = _context.db;
        DropBranchStatement *stmt = _context.parserResult.dropBranchStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        branch_id_t branch = lookupBranch(db, stmt->branchName);
        if (branch == master_branch_id) throw semantic_sql_error("the root branch cannot be dropped");
        auto subtree = db.getSubtree(branch);
        if (subtree.size() > 1 && !stmt->cascade) {
            std::string children;
            for (size_t i = 1; i < subtree.size(); ++i) children += (i > 1 ? ", " : "") + db._branches.at(subtree[i])->name;
            throw semantic_sql_error("branch '" + stmt->branchName + "' has descendants (" + children + "); use DROP BRANCH " +
                                     stmt->branchName + " CASCADE");
        }
    }

    void DropBranchAnalyser::constructTree() {
        Database &db = _context.db;
        DropBranchStatement *stmt = _context.parserResult.dropBranchStmt;
        auto subtree = db.getSubtree(lookupBranch(db, stmt->branchName));

        size_t freed = drop_branches(db, subtree);

        std::cout << "Dropped branch " << stmt->branchName;
        if (subtree.size() > 1) std::cout << " and " << (subtree.size() - 1) << " descendant" << (subtree.size() > 2 ? "s" : "");
        std::cout << ", freed " << freed << " version" << (freed != 1 ? "s" : "") << "\n";
        _context.joinedTree = nullptr;
    }

    //
    // MERGE [ BRANCH ] b INTO p
    //

    void MergeBranchAnalyser::verify() {
        Database &db = _context.db;
        MergeBranchStatement *stmt = _context.parserResult.mergeBranchStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        branch_id_t source = lookupBranch(db, stmt->branchName);
        branch_id_t target = lookupBranch(db, stmt->targetBranchName);
        if (source == target) throw semantic_sql_error("a branch cannot be merged into itself");
    }

    void MergeBranchAnalyser::constructTree() {
        Database &db = _context.db;
        MergeBranchStatement *stmt = _context.parserResult.mergeBranchStmt;

        MergePolicy policy = stmt->onConflict == "target" ? MergePolicy::KeepTarget
                           : stmt->onConflict == "source" ? MergePolicy::KeepSource : MergePolicy::Fail;
        MergeResult result = merge_branch(db, lookupBranch(db, stmt->branchName), lookupBranch(db, stmt->targetBranchName), policy);
        if (!result.conflicts.empty()) {
            const size_t shown = std::min<size_t>(result.conflicts.size(), 10);
            std::string message = "merge of '" + stmt->branchName + "' into '" + stmt->targetBranchName + "' failed with " +
                                  std::to_string(result.conflicts.size()) + " conflict" + (result.conflicts.size() > 1 ? "s" : "") + ":";
            for (size_t i = 0; i < shown; ++i) message += "\n  " + result.conflicts[i];
            if (shown < result.conflicts.size()) message += "\n  ...";
            throw semantic_sql_error(message);
        }

        std::cout << "Merged " << stmt->branchName << " into " << stmt->targetBranchName << ": " << result.updated << " updated, "
                  << result.inserted << " inserted, " << result.deleted << " deleted";
        if (result.resolved > 0) std::cout << ", " << result.resolved << " conflicts kept the " << stmt->onConflict;
        std::cout << "\n";
        _context.changedTuples = result.updated + result.inserted + result.deleted;
        if (result.resolved > 0) {
            _context.notice = std::to_string(result.resolved) + " conflict" + (result.resolved > 1 ? "s" : "") + " kept the " + stmt->onConflict;
        }
        _context.joinedTree = nullptr;
    }

    //
    // ALTER TABLE t [ VERSION b ] ADD [ COLUMN ] c type [ NOT NULL ] DEFAULT v, DROP [ COLUMN ] c
    //

    void AlterTableAnalyser::verify() {
        Database &db = _context.db;
        AlterTableStatement *stmt = _context.parserResult.alterTableStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        Table *table = db.getTable(stmt->relation.name);
        if (table == nullptr) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        branch_id_t branch = lookupBranch(db, stmt->relation.version);
        const auto &column = stmt->column;
        if (stmt->add) {
            if (table->findColumn(column.name, branch) != nullptr)
                throw semantic_sql_error("column '" + column.name + "' already exists in branch '" + stmt->relation.version + "'");
            if (!isColumnType(column.type)) throw semantic_sql_error("type '" + column.type + "' does not exist");
            // values of these types cannot be cast from strings yet
            if (column.type == "longinteger" || column.type == "char")
                throw semantic_sql_error("columns of type " + column.type + " cannot be added yet");
            // versions hold no null indicators, so the column has a value in every version
            if (!stmt->hasDefault)
                throw semantic_sql_error("column '" + column.name + "' needs a DEFAULT, which the existing tuples take");
            try {
                ColumnSpec spec = column;
                spec.nullable = false;
                Native::Sql::Value::castString(stmt->defaultValue, toColumnType(spec));
            } catch (const std::exception &) {
                throw semantic_sql_error("'" + stmt->defaultValue + "' is no value of type " + column.type);
            }
        } else {
            if (table->findColumn(column.name, branch) == nullptr)
                throw semantic_sql_error("column '" + column.name + "' does not exist in branch '" + stmt->relation.version + "'");
            if (table->getSchema(branch).size() == 1) throw semantic_sql_error("the last column of a table cannot be dropped");
        }
    }

    void AlterTableAnalyser::constructTree() {
        Database &db = _context.db;
        AlterTableStatement *stmt = _context.parserResult.alterTableStmt;
        Table *table = db.getTable(stmt->relation.name);
        branch_id_t branch = lookupBranch(db, stmt->relation.version);
        const std::string &branchName = db._branches.at(branch)->name;

        if (stmt->add) {
            ColumnSpec spec = stmt->column;
            spec.nullable = false;
            const Sql::SqlType type = toColumnType(spec);
            auto defaultValue = Native::Sql::Value::castString(stmt->defaultValue, type);
            add_branch_column(*table, branch, spec.name, type, *defaultValue);
            std::cout << "Added column " << spec.name << " to " << stmt->relation.name << " in branch " << branchName << "\n";
        } else {
            table->dropBranchColumn(branch, table->findColumn(stmt->column.name, branch)->index);
            std::cout << "Dropped column " << stmt->column.name << " of " << stmt->relation.name << " in branch " << branchName << "\n";
        }
        _context.joinedTree = nullptr;
    }

}
