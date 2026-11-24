//
// Created by Blum Thomas on 2020-06-29.
//
// COPY t [ VERSION b ] FROM|TO 'file' with the options of CopyFormat

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "semanticAnalyser/CopyFormat.hpp"
#include "foundations/version_management.hpp"
#include "queryCompiler/QueryContext.hpp"

#include <fstream>

namespace semanticalAnalysis {

    static branch_id_t copyBranch(Database & db, const CopyStatement & stmt) {
        auto branch = db._branchMapping.find(stmt.relation.version);
        if (branch == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt.relation.version + "' does not exist");
        return branch->second;
    }

    void CopyTableAnalyser::verify() {
        Database &db = _context.db;
        CopyStatement* stmt = _context.parserResult.copyStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name))
            throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        copyBranch(db, *stmt);
        CopyFormat format(*stmt); // checks the options
    }

    /// COPY FROM reads the file twice: first it checks every record, so that an error leaves the table as it
    /// was, then it inserts the records one at a time. \returns The number of records
    static uint64_t copyFrom(Database & db, Table & table, branch_id_t branch, const CopyStatement & stmt, const CopyFormat & format) {
        std::vector<ci_p_t> columns;
        for (size_t column : table.getSchema(branch)) columns.push_back(table.getCI(column));
        QueryContext context(db);
        uint64_t rows = 0;
        std::vector<CopyField> fields;
        std::vector<std::pair<ci_p_t, Native::Sql::value_op_t>> values;
        for (const bool inserting : {false, true}) {
            std::ifstream in(stmt.filePath, std::ios::binary);
            if (!in) throw semantic_sql_error("could not open file \"" + stmt.filePath + "\" for reading");
            CopyReader reader(in, format);
            if (format.header) reader.next(fields);
            while (reader.next(fields)) {
                auto where = [&](const std::string & message) {
                    return "COPY " + stmt.relation.name + ", line " + std::to_string(reader.line()) + ": " + message;
                };
                // a tbl line may end with the delimiter
                if (!format.csv && fields.size() == columns.size() + 1 && fields.back().value.empty()) fields.pop_back();
                if (fields.size() < columns.size()) {
                    throw semantic_sql_error(where("missing data for column \"" + columns[fields.size()]->columnName + "\""));
                }
                if (fields.size() > columns.size()) throw semantic_sql_error(where("extra data after last expected column"));
                values.clear();
                for (size_t i = 0; i < columns.size(); ++i) {
                    const ci_p_t ci = columns[i];
                    const CopyField & field = fields[i];
                    // the text of NULL is NULL in a nullable column; in a column of strings declared NOT NULL it
                    // is a string like any other (an empty field is the empty string)
                    const bool nullText = !field.quoted && field.value == format.null;
                    const auto type = ci->type.typeID;
                    if (nullText && !ci->type.nullable && type != Sql::SqlType::TypeID::CharID &&
                        type != Sql::SqlType::TypeID::VarcharID && type != Sql::SqlType::TypeID::TextID) {
                        throw not_null_violation(where("null value in column \"" + ci->columnName + "\" violates not-null constraint"));
                    }
                    const bool isNull = nullText && ci->type.nullable;
                    // any text is a valid text value; the check does not put it into the string pool
                    if (!inserting && (isNull || type == Sql::SqlType::TypeID::TextID)) continue;
                    try {
                        values.emplace_back(ci, isNull ? Native::Sql::NullableValue::getNull(ci->type)
                                                       : Native::Sql::castColumnValue(field.value, ci->type));
                    } catch (const std::exception & e) {
                        throw semantic_sql_error(where("column \"" + ci->columnName + "\": " + e.what()));
                    } catch (const char * message) { // the parsers of numbers throw their message
                        throw semantic_sql_error(where("column \"" + ci->columnName + "\": " + message));
                    }
                }
                if (inserting) {
                    auto tuple = complete_tuple(table, std::move(values));
                    insert_tuple_with_branchId(*tuple, table, context, branch);
                    ++rows;
                }
            }
            if (in.bad()) throw semantic_sql_error("could not read file \"" + stmt.filePath + "\"");
        }
        return rows;
    }

    void CopyTableAnalyser::constructTree() {
        Database &db = _context.db;
        CopyStatement* stmt = _context.parserResult.copyStmt;
        Table *table = db.getTable(stmt->relation.name);
        const branch_id_t branch = copyBranch(db, *stmt);
        const CopyFormat format(*stmt);

        if (stmt->directionFrom) {
            // runs here, without a plan
            _context.changedTuples = copyFrom(db, *table, branch, *stmt, format);
            return;
        }

        // the columns of the branch in their order, as COPY FROM reads them
        const std::vector<std::string> names = table->getColumnNames(branch);
        _context.copyWriter = std::make_shared<CopyWriter>(stmt->filePath, format, names);

        construct_scans(_context, stmt->relation);
        auto &production = _context.dangling_productions[stmt->relation.name];
        std::vector<iu_p_t> projectedIUs;
        std::string &productionName = stmt->relation.alias.empty() ? stmt->relation.name : stmt->relation.alias;
        for (auto &name : names) {
            projectedIUs.push_back(_context.ius[productionName].at(name));
        }
        _context.joinedTree = std::make_unique<Result>( std::move(production), projectedIUs );
    }

}
