//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "foundations/loader.hpp"

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
        if (!fs) { throw std::runtime_error("file not found"); }
        Table *table = _context.db.getTable(stmt->relation.name);

        char delimiter = ';';
        if (stmt->format.compare("tbl") == 0) delimiter = '|';
        loadTable(fs, *table, delimiter);

        _context.joinedTree = nullptr;
    }

}