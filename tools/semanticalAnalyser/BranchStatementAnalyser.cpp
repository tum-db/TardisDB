// CREATE INDEX ... VERSION b, DROP BRANCH and MERGE BRANCH

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
        const auto columnNames = table->getColumnNames();
        auto it = std::find(columnNames.begin(), columnNames.end(), stmt->columns.front().name);
        if (it == columnNames.end()) throw semantic_sql_error("column '" + stmt->columns.front().name + "' does not exist");
        if (table->getIndex(it - columnNames.begin(), branch) != nullptr)
            throw semantic_sql_error("branch '" + stmt->relation.version + "' already has an index on " + stmt->relation.name + " (" + *it + ")");
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
        const auto columnNames = table->getColumnNames();
        const std::string &column = stmt->columns.front().name;
        const size_t position = std::find(columnNames.begin(), columnNames.end(), column) - columnNames.begin();

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

        MergeResult result = merge_branch(db, lookupBranch(db, stmt->branchName), lookupBranch(db, stmt->targetBranchName));
        if (!result.conflicts.empty()) {
            const size_t shown = std::min<size_t>(result.conflicts.size(), 10);
            std::string message = "merge of '" + stmt->branchName + "' into '" + stmt->targetBranchName + "' failed with " +
                                  std::to_string(result.conflicts.size()) + " conflict" + (result.conflicts.size() > 1 ? "s" : "") + ":";
            for (size_t i = 0; i < shown; ++i) message += "\n  " + result.conflicts[i];
            if (shown < result.conflicts.size()) message += "\n  ...";
            throw semantic_sql_error(message);
        }

        std::cout << "Merged " << stmt->branchName << " into " << stmt->targetBranchName << ": " << result.updated << " updated, "
                  << result.inserted << " inserted, " << result.deleted << " deleted\n";
        _context.joinedTree = nullptr;
    }

}
