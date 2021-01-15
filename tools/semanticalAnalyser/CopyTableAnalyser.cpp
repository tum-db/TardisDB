//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "foundations/loader.hpp"
#include "queryCompiler/QueryContext.hpp"

#include <fstream>

namespace semanticalAnalysis {

    static std::string path;
    void CopyTableAnalyser::dumpCallbackCSV(Native::Sql::SqlTuple *tuple) {
        std::ofstream output(path,std::fstream::ios_base::app);
        for (int i = 0; i<tuple->values.size()-1; i++) {
            output << Native::Sql::toString(*tuple->values[i]);
            output << ";";
        }
        output << Native::Sql::toString(*tuple->values[tuple->values.size()-1]) << "\n";
        output.close();
    }
    void CopyTableAnalyser::dumpCallbackTBL(Native::Sql::SqlTuple *tuple) {
        std::ofstream output(path,std::fstream::ios_base::app);
        for (int i = 0; i<tuple->values.size()-1; i++) {
            output << Native::Sql::toString(*tuple->values[i]);
            output << "|";
        }
        output << Native::Sql::toString(*tuple->values[tuple->values.size()-1]) << "\n";
        output.close();
    }

    void CopyTableAnalyser::verify() {
        Database &db = _context.db;
        CopyStatement* stmt = _context.parserResult.copyStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name))
            throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        std::set<std::string> formats = { "csv" , "tbl" };
        if (std::find(formats.begin(),formats.end(),stmt->format) == formats.end())
            throw semantic_sql_error("format type '" + stmt->format + "' does not exist");
        if (!stmt->directionFrom && db._branchMapping.find(stmt->relation.version) == db._branchMapping.end())
            throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
    }

    void CopyTableAnalyser::constructTree() {
        CopyStatement* stmt = _context.parserResult.copyStmt;
        if (stmt->directionFrom) {
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
        } else {
            path = stmt->filePath;
            remove(stmt->filePath.c_str());
            _context.callback = (stmt->format.compare("tbl") == 0) ? (void*)&CopyTableAnalyser::dumpCallbackTBL :
                                (void*)&CopyTableAnalyser::dumpCallbackCSV;

            construct_scans(_context, stmt->relation);
            auto &production = _context.dangling_productions[stmt->relation.name];

            std::vector<iu_p_t> projectedIUs;
            std::string &productionName = stmt->relation.alias.empty() ? stmt->relation.name : stmt->relation.alias;
            for (auto &iu : _context.ius[productionName]) {
                if (iu.second->columnInformation->columnName == "tid") continue;
                projectedIUs.push_back(iu.second);
            }

            _context.joinedTree = std::make_unique<Result>( std::move(production), projectedIUs );
        }
    }

}
