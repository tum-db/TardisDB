#include "queryCompiler/QueryContext.hpp"

void ExecutionContext::acquireResource(std::unique_ptr<ExecutionResource> && resource)
{
    resources.push_back(std::move(resource));
}

void QueryContext::constructBranchLineages(std::set<branch_id_t> &branches, QueryContext &context) {
    for (auto &branch : branches) {
        context.analyzingContext.db.constructBranchLineage(branch,context.executionContext);
    }
}

#if USE_HYRISE

std::tuple<std::string,int,int> QueryContext::convertDataType(hsql::ColumnType type) {
    switch (type.data_type) {
        case hsql::DataType::UNKNOWN:
            return std::make_tuple("",0,0);
        case hsql::DataType::CHAR:
            return std::make_tuple("char",type.length,0);
        case hsql::DataType::VARCHAR:
            return std::make_tuple("varchar",type.length,0);
        case hsql::DataType::DOUBLE:
            return std::make_tuple("numeric",64,16);
        case hsql::DataType::FLOAT:
            return std::make_tuple("numeric",32,8);
        case hsql::DataType::INT:
            return std::make_tuple("integer",0,0);
        case hsql::DataType::LONG:
            return std::make_tuple("longinteger",0,0);
        case hsql::DataType::TEXT:
            return std::make_tuple("text",0,0);
    }
}

void QueryContext::collectTables(std::vector<semanticalAnalysis::Relation> &relations, hsql::TableRef *tableRef) {
    switch (tableRef->type) {
        case hsql::TableRefType::kTableName:
            relations.push_back(semanticalAnalysis::Relation());
            relations.back().name = tableRef->name;
            relations.back().alias = (tableRef->alias != nullptr) ? tableRef->alias->name : "";
            relations.back().version = (tableRef->version != nullptr) ? tableRef->version->name : "master";
            break;
        case hsql::TableRefType::kTableCrossProduct:
            for (auto table : *tableRef->list) {
                collectTables(relations,table);
            }
            break;
        default:
            break;
    }
}

std::string QueryContext::expressionValueToString(hsql::Expr *expr) {
    switch (expr->type) {
        case hsql::ExprType::kExprLiteralInt:
            return std::to_string(expr->ival);
        case hsql::ExprType::kExprLiteralString:
            return expr->name;
        case hsql::ExprType::kExprLiteralFloat:
            return std::to_string(expr->fval);
        default:
            break;
    }
    return "";
}

void QueryContext::collectSelections(std::vector<std::pair<semanticalAnalysis::Column,std::string>> &selections, hsql::Expr *whereClause) {
    switch (whereClause->type) {
        case hsql::ExprType::kExprOperator:
            if (whereClause->opType == hsql::OperatorType::kOpAnd) {
                collectSelections(selections,whereClause->expr);
                collectSelections(selections,whereClause->expr2);
            } else if (whereClause->opType == hsql::OperatorType::kOpEquals) {
                if (whereClause->expr->type == hsql::ExprType::kExprColumnRef) {
                    if (whereClause->expr2->type == hsql::ExprType::kExprLiteralInt) {
                        selections.push_back(std::make_pair(semanticalAnalysis::Column(),expressionValueToString(whereClause->expr2)));
                        selections.back().first.name = whereClause->expr->name;
                        selections.back().first.table = (whereClause->expr->table != nullptr) ? whereClause->expr->table : "";
                    }
                }
            }
            break;
        default:
            break;
    }
}

void QueryContext::collectJoinConditions(std::vector<std::pair<semanticalAnalysis::Column,semanticalAnalysis::Column>> &joinConditions, hsql::Expr *whereClause) {
    switch (whereClause->type) {
        case hsql::ExprType::kExprOperator:
            if (whereClause->opType == hsql::OperatorType::kOpAnd) {
                collectJoinConditions(joinConditions,whereClause->expr);
                collectJoinConditions(joinConditions,whereClause->expr2);
            } else if (whereClause->opType == hsql::OperatorType::kOpEquals) {
                if (whereClause->expr->type == hsql::ExprType::kExprColumnRef) {
                    if (whereClause->expr2->type == hsql::ExprType::kExprColumnRef) {
                        joinConditions.push_back(std::make_pair(semanticalAnalysis::Column(),semanticalAnalysis::Column()));
                        joinConditions.back().first.table = (whereClause->expr->table != nullptr) ? whereClause->expr->table : "";
                        joinConditions.back().first.name = whereClause->expr->name;
                        joinConditions.back().second.table = (whereClause->expr2->table != nullptr) ? whereClause->expr2->table : "";
                        joinConditions.back().second.name = whereClause->expr2->name;
                    }
                }
            }
            break;
        default:
            break;
    }
}

void QueryContext::convertToParserResult(semanticalAnalysis::SQLParserResult &dest, hsql::SQLParserResult &source) {
    if (source.isValid()) {
        const hsql::SQLStatement *stmt = source.getStatement(0);
        const hsql::CreateStatement *_create;
        const hsql::CreateBranchStatement *_createbranch;
        const hsql::SelectStatement *_select;
        const hsql::InsertStatement *_insert;
        const hsql::UpdateStatement *_update;
        const hsql::DeleteStatement *_delete;
        const hsql::ImportStatement *_import;
        switch (stmt->type()) {
            case hsql::kStmtCreate:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateTable;
                dest.createTableStmt = new semanticalAnalysis::CreateTableStatement();

                _create = (hsql::CreateStatement*)stmt;
                dest.createTableStmt->tableName = _create->tableName;
                for (auto &column : *_create->columns) {
                    dest.createTableStmt->columns.push_back(semanticalAnalysis::ColumnSpec());
                    dest.createTableStmt->columns.back().name = column->name;
                    auto [typeStr,length,precision] = convertDataType(column->type);
                    dest.createTableStmt->columns.back().type = typeStr;
                    dest.createTableStmt->columns.back().length = length;
                    dest.createTableStmt->columns.back().precision = precision;
                    dest.createTableStmt->columns.back().nullable = column->nullable;
                }
                break;
            case hsql::kStmtCreateBranch:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateBranch;
                dest.createBranchStmt = new semanticalAnalysis::CreateBranchStatement();

                _createbranch = (hsql::CreateBranchStatement*)stmt;
                dest.createBranchStmt->branchName = _createbranch->name;
                dest.createBranchStmt->parentBranchName = _createbranch->parent;
                break;
            case hsql::kStmtBranch:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::Branch;
                break;
            case hsql::kStmtSelect:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
                dest.selectStmt = new semanticalAnalysis::SelectStatement();

                _select = (hsql::SelectStatement*)stmt;
                collectTables(dest.selectStmt->relations,_select->fromTable);
                if (_select->whereClause != nullptr) {
                    collectSelections(dest.selectStmt->selections,_select->whereClause);
                    collectJoinConditions(dest.selectStmt->joinConditions,_select->whereClause);
                }
                for (auto projection : *_select->selectList) {
                    if (projection->type != hsql::ExprType::kExprColumnRef) continue;
                    dest.selectStmt->projections.push_back(semanticalAnalysis::Column());
                    dest.selectStmt->projections.back().name = projection->name;
                }
                break;
            case hsql::kStmtInsert:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::Insert;
                dest.insertStmt = new semanticalAnalysis::InsertStatement();

                _insert = (hsql::InsertStatement*)stmt;
                dest.insertStmt->relation.name = _insert->tableName;
                dest.insertStmt->relation.version = (_insert->version != nullptr) ? _insert->version->name : "master";
                if (_insert->columns != nullptr) {
                    for (auto column : *_insert->columns) {
                        dest.insertStmt->columns.push_back(semanticalAnalysis::Column());
                        dest.insertStmt->columns.back().name = column;
                    }
                }
                if (_insert->values != nullptr) {
                    for (auto value : *_insert->values) {
                        dest.insertStmt->values.push_back(expressionValueToString(value));
                    }
                }
                break;
            case hsql::kStmtUpdate:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::Update;
                dest.updateStmt = new semanticalAnalysis::UpdateStatement();

                _update = (hsql::UpdateStatement*)stmt;
                dest.updateStmt->relation.name = _update->table->name;
                dest.updateStmt->relation.version = (_update->table->version != nullptr) ? _update->table->version->name : "master";
                if (_update->where != nullptr) collectSelections(dest.updateStmt->selections,_update->where);
                for (auto update : *_update->updates) {
                    dest.updateStmt->updates.push_back(std::make_pair(semanticalAnalysis::Column(),expressionValueToString(update->value)));
                    dest.updateStmt->updates.back().first.name = update->column;
                }
                break;
            case hsql::kStmtDelete:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::Delete;
                dest.deleteStmt = new semanticalAnalysis::DeleteStatement();

                _delete = (hsql::DeleteStatement*)stmt;
                dest.deleteStmt->relation.name = _delete->tableName;
                dest.deleteStmt->relation.version = (_delete->version != nullptr) ? _delete->version->name : "master";
                if (_delete->expr != nullptr) collectSelections(dest.deleteStmt->selections,_delete->expr);
                break;
            case hsql::kStmtImport:
                dest.opType = semanticalAnalysis::SQLParserResult::OpType::Copy;
                dest.copyStmt = new semanticalAnalysis::CopyStatement();

                _import = (hsql::ImportStatement*)stmt;
                dest.copyStmt->relation.name = _import->tableName;
                dest.copyStmt->filePath = _import->filePath;
                switch (_import->type) {
                    case hsql::ImportType::kImportCSV:
                        dest.copyStmt->format = "csv";
                        break;
                    case hsql::ImportType::kImportTbl:
                        dest.copyStmt->format = "tbl";
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }
}

#else

void QueryContext::convertToParserResult(semanticalAnalysis::SQLParserResult &dest, tardisParser::ParsingContext &source) {
    dest = std::move((semanticalAnalysis::SQLParserResult&)source);
    switch (source.opType) {
        case tardisParser::ParsingContext::Unkown:
            break;
        case tardisParser::ParsingContext::CreateTable:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateTable;
            break;
        case tardisParser::ParsingContext::CreateBranch:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateBranch;
            break;
        case tardisParser::ParsingContext::Branch:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Branch;
            break;
        case tardisParser::ParsingContext::Insert:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Insert;
            break;
        case tardisParser::ParsingContext::Select:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
            break;
        case tardisParser::ParsingContext::Update:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Update;
            break;
        case tardisParser::ParsingContext::Delete:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Delete;
            break;
        case tardisParser::ParsingContext::Copy:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Copy;
            break;
    }
    source = tardisParser::ParsingContext();
}

#endif