//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "foundations/loader.hpp"

#include <fstream>

namespace semanticalAnalysis {

    void DumpTableAnalyser::verify() {
        Database &db = _context.db;
        DumpStatement* stmt = _context.parserResult.dumpStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name))
            throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end())
            throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
    }

    void DumpTableAnalyser::constructTree() {
        DumpStatement* stmt = _context.parserResult.dumpStmt;

        std::string &filePath = stmt->filePath;
        remove(filePath.c_str());
        _context.callback = new std::function<void(Native::Sql::SqlTuple *)>([&filePath](Native::Sql::SqlTuple *tuple) {
            std::ofstream output(filePath,std::fstream::ios_base::app);
            for (int i = 0; i<tuple->values.size()-1; i++) {
                output << Native::Sql::toString(*tuple->values[i]);
                output << ";";
            }
            output << Native::Sql::toString(*tuple->values[tuple->values.size()-1]) << "\n";
            output.close();
        });

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