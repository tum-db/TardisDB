//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "foundations/loader.hpp"
#include "queryCompiler/QueryContext.hpp"

#include <fstream>

namespace semanticalAnalysis {

    void CopyTableAnalyser::verify() {
        Database &db = _context.db;
        CopyStatement* stmt = _context.parserResult.copyStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name))
            throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        std::set<std::string> formats = { "csv" , "tbl" };
        if (std::find(formats.begin(),formats.end(),stmt->format) == formats.end())
            throw semantic_sql_error("format type '" + stmt->format + "' does not exist");
    }

    void CopyTableAnalyser::constructTree() {
        CopyStatement* stmt = _context.parserResult.copyStmt;
        std::ifstream fs(stmt->filePath);
        auto& db = _context.db;

        if (!fs) { throw std::runtime_error("file not found"); }
        Table *table = db.getTable(stmt->relation.name);

        std::string &branchName = stmt->relation.version;
        branch_id_t branchId = (branchName.compare("master") != 0) ?
                db._branchMapping[branchName]:
                master_branch_id;

        char delimiter = ';';
        if (stmt->format.compare("tbl") == 0) delimiter = '|';
        QueryContext queryContext(db);

        std::vector<Native::Sql::SqlTuple *> tuples;
        std::string rowStr;
        while (std::getline(fs, rowStr)) {
            std::vector<std::string> items = split(rowStr, delimiter);
            size_t i = 0;
            std::vector<Native::Sql::value_op_t> values;
            for (const std::string & column : table->getColumnNames()) {
                ci_p_t ci = table->getCI(column);
                auto value = Native::Sql::Value::castString(items[i], ci->type);
                values.push_back(std::move(value));
                i += 1;
            }
            tuples.push_back(new Native::Sql::SqlTuple(std::move(values)));
        }
        _context.joinedTree = std::make_unique<Insert>(_context.iuFactory,*table,move(tuples),branchId);
    }

}