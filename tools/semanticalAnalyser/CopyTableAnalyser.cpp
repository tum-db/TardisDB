//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "foundations/loader.hpp"

#include <fstream>

namespace semanticalAnalysis {

    static std::string path;
    void CopyTableAnalyser::dumpCallback(Native::Sql::SqlTuple *tuple) {
        std::ofstream output(path,std::fstream::ios_base::app);
        for (int i = 0; i<tuple->values.size()-1; i++) {
            output << Native::Sql::toString(*tuple->values[i]);
            output << ";";
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
            if (!fs) { throw std::runtime_error("file not found"); }
            Table *table = _context.db.getTable(stmt->relation.name);

            char delimiter = ';';
            if (stmt->format.compare("tbl") == 0) delimiter = '|';
            loadTable(fs, *table, delimiter);

            _context.joinedTree = nullptr;
        } else {
            path = stmt->filePath;
            remove(stmt->filePath.c_str());
            _context.callback = (void*)&CopyTableAnalyser::dumpCallback;

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