#include <algorithm>

#include "sqlParser/SQLParser.hpp"

namespace tardisParser {
    // Parses an identifier into a binding and its attribute
    BindingAttribute SQLParser::parse_binding_attribute(std::string value) {
        auto dotPos = value.find_first_of(".");
        if (dotPos == std::string::npos) {
            // throw incorrect_sql_error("invalid binding attribute");
            return BindingAttribute({}, value);
        }

        auto binding = value.substr(0, dotPos);
        auto attribute = value.substr(dotPos + 1, value.size());
        return BindingAttribute(binding, attribute);
    }

    void SQLParser::parseToken(Tokenizer &tokenizer, ParsingContext &context) {
        const Token &token = tokenizer.next();
        // When the token is a delimiter check if the parser is in a valid final state
        if (token.hasType(Type::delimiter)) {
            if (!context.stateIsFinal()) {
                throw syntactical_error("unexpected end of input");
            }
            context.state = State::Done;
            return;
        }

        // Implements state transitions
        switch (context.state) {
            case State::Init:
                if (token.equalsKeyword(Keyword::Select)) {
                    context.opType = ParsingContext::OpType::Select;
                    context.selectStmt = new SelectStatement();
                    context.state = State::Select;
                } else if (token.equalsKeyword(Keyword::Insert)) {
                    context.opType = ParsingContext::OpType::Insert;
                    context.insertStmt = new InsertStatement();
                    context.state = State::Insert;
                } else if (token.equalsKeyword(Keyword::Update)) {
                    context.opType = ParsingContext::OpType::Update;
                    context.updateStmt = new UpdateStatement();
                    context.state = State::Update;
                } else if (token.equalsKeyword(Keyword::Delete)) {
                    context.opType = ParsingContext::OpType::Delete;
                    context.deleteStmt = new DeleteStatement();
                    context.state = State::Delete;
                } else if (token.equalsKeyword(Keyword::Create)) {
                    context.state = State::Create;
                } else if (token.equalsKeyword(Keyword::Branch)) {
                    context.opType = ParsingContext::OpType::Branch;
                    context.state = State::Branch;
                } else if (token.equalsKeyword(Keyword::Copy)) {
                    context.opType = ParsingContext::OpType::Copy;
                    context.copyStmt = new CopyStatement();
                    context.state = State::Copy;
                } else if (token.equalsKeyword(Keyword::Dump)) {
                    context.opType = ParsingContext::OpType::Dump;
                    context.dumpStmt = new DumpStatement();
                    context.state = State::Dump;
                } else {
                    throw syntactical_error("Expected 'Select', 'Insert', 'Update', 'Delete' , 'BRANCH', 'COPY' or 'Create', found '" + token.value + "'");
                }
                break;

                //
                // Dump
                //
            case State::Dump:
                if (token.hasType(Type::identifier)) {
                    context.dumpStmt->relation.name = token.value;
                    context.state = State::DumpTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::DumpTable:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::DumpTableVersion;
                } else if (token.equalsKeyword(Keyword::To)) {
                    context.state = State::DumpTo;
                    context.dumpStmt->relation.version = "master";
                } else {
                    throw syntactical_error("Expected 'VERSION' or 'TO', found '" + token.value + "'");
                }
                break;
            case State::DumpTableVersion:
                if (token.hasType(Type::identifier)) {
                    context.dumpStmt->relation.version = token.value;
                    context.state = State::DumpTableTag;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::DumpTableTag:
                if (token.equalsKeyword(Keyword::To)) {
                    context.state = State::DumpTo;
                } else {
                    throw syntactical_error("Expected 'TO', found '" + token.value + "'");
                }
                break;
            case State::DumpTo:
                if (token.hasType(Type::literal)) {
                    context.dumpStmt->filePath = token.value;
                    context.state = State::DumpFile;
                } else {
                    throw syntactical_error("Expected file path, found '" + token.value + "'");
                }
                break;

                //
                //  Copy
                //
            case State::Copy:
                if (token.hasType(Type::identifier)) {
                    context.copyStmt->relation.name = token.value;
                    context.state = State::CopyTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CopyTable:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::CopyFrom;
                } else {
                    throw syntactical_error("Expected 'FROM', found '" + token.value + "'");
                }
                break;
            case State::CopyFrom:
                if (token.hasType(Type::literal)) {
                    context.copyStmt->filePath = token.value;
                    context.state = State::CopyPath;
                } else {
                    throw syntactical_error("Expected file path, found '" + token.value + "'");
                }
                break;
            case State::CopyPath:
                if (token.equalsKeyword(Keyword::With)) {
                    context.state = State::CopyWith;
                } else {
                    throw syntactical_error("Expected 'WITH', found '" + token.value + "'");
                }
                break;
            case State::CopyWith:
                if (token.equalsKeyword(Keyword::Format)) {
                    context.state = State::CopyFormat;
                } else {
                    throw syntactical_error("Expected 'FORMAT', found '" + token.value + "'");
                }
                break;
            case State::CopyFormat:
                if (token.equalsKeyword(Keyword::CSV)) {
                    context.copyStmt->format = "csv";
                    context.state = State::CopyType;
                } else if (token.equalsKeyword(Keyword::TBL)) {
                    context.copyStmt->format = "tbl";
                    context.state = State::CopyType;
                } else {
                    throw syntactical_error("Expected format name, found '" + token.value + "'");
                }
                break;

                //
                //  CREATE
                //
            case State::Create:
                if (token.equalsKeyword(Keyword::Table)) {
                    context.opType = ParsingContext::OpType::CreateTable;
                    context.createTableStmt = new CreateTableStatement();
                    context.state = State::CreateTable;
                } else if (token.equalsKeyword(Keyword::Branch)) {
                    context.opType = ParsingContext::OpType::CreateBranch;
                    context.createBranchStmt = new CreateBranchStatement();
                    context.state = State::CreateBranch;
                } else {
                    throw syntactical_error("Expected 'TABLE' or 'Branch', found '" + token.value + "'");
                }
                break;
            case State::CreateBranch:
                if (token.type == Type::identifier) {
                    context.createBranchStmt->branchName = token.value;
                    context.state = State::CreateBranchTag;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::CreateBranchTag:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::CreateBranchFrom;
                } else {
                    throw syntactical_error("Expected 'FROM', found '" + token.value + "'");
                }
                break;
            case State::CreateBranchFrom:
                if (token.type == Type::identifier) {
                    context.createBranchStmt->parentBranchName = token.value;
                    context.state = State::CreateBranchParent;
                } else {
                    throw syntactical_error("Expected parent branch name, found '" + token.value + "'");
                }
                break;

            case State::CreateTable:
                if (token.type == Type::identifier) {
                    context.createTableStmt->tableName = token.value;
                    context.state = State::CreateTableRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableRelationName:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::CreateTableColumnsBegin;
                }
                break;
            case State::CreateTableColumnsBegin:
            case State::CreateTableColumnSeperator:
                if (token.type == Type::identifier) {
                    context.createTableStmt->columns.push_back(ColumnSpec());
                    context.createTableStmt->columns.back().name = token.value;
                    context.state = CreateTableColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableColumnName:
                if (token.type == Type::identifier) {
                    std::string lowercase_token_value;
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(lowercase_token_value), tolower);
                    context.createTableStmt->columns.back().type = lowercase_token_value;
                    context.state = CreateTableColumnType;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableColumnType:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = CreateTableTypeDetailBegin;
                    break;
                }
                context.createTableStmt->columns.back().length = 0;
                context.createTableStmt->columns.back().precision = 0;
                if (token.equalsKeyword(Keyword::Not)) {
                    context.state = CreateTableTypeNot;
                    break;
                }
                context.createTableStmt->columns.back().nullable = true;

                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableColumnSeperator;
                } else {
                    throw syntactical_error("Expected '(' , ',' , ')' or 'NOT', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailBegin:
                if (token.type == literal && std::isdigit(token.value[0])) {
                    context.createTableStmt->columns.back().length = std::stoi(token.value);
                    context.state = CreateTableTypeDetailLength;
                } else {
                    throw syntactical_error("Expected type length, found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailLength:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.createTableStmt->columns.back().precision = 0;
                    context.state = CreateTableTypeDetailEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableTypeDetailSeperator;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailSeperator:
                if (token.type == literal && std::isdigit(token.value[0])) {
                    context.createTableStmt->columns.back().precision = std::stoi(token.value);
                    context.state = CreateTableTypeDetailPrecision;
                } else {
                    throw syntactical_error("Expected type precision, found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailPrecision:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableTypeDetailEnd;
                } else {
                    throw syntactical_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailEnd:
                if (token.equalsKeyword(Keyword::Not)) {
                    context.state = CreateTableTypeNot;
                    break;
                }
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableColumnSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')' or 'NOT', found '" + token.value + "'");
                }
                context.createTableStmt->columns.back().nullable = true;
                break;
            case State::CreateTableTypeNot:
                if (token.equalsKeyword(Keyword::Null)) {
                    context.createTableStmt->columns.back().nullable = false;
                    context.state = State::CreateTableTypeNotNull;
                } else {
                    throw syntactical_error("Expected 'NULL', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeNotNull:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableColumnSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;

                //
                //  DELETE
                //
            case State::Delete:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::DeleteFrom;
                } else {
                    throw syntactical_error("Expected 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteFrom:
                if (token.type == Type::identifier) {
                    context.deleteStmt->relation.name = token.value;
                    context.deleteStmt->relation.version = "master";
                    context.state = DeleteRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::DeleteRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::DeleteVersion;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::DeleteWhere;
                } else {
                    throw syntactical_error("Expected 'VERSION' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteVersion:
                if (token.type == Type::identifier) {
                    context.deleteStmt->relation.version = token.value;
                    context.state = DeleteTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::DeleteTag:
                if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::DeleteWhere;
                } else {
                    throw syntactical_error("Expected 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteWhere:
            case State::DeleteWhereAnd:
                if (token.type == Type::identifier) {
                    context.state = State::DeleteWhereExprLhs;
                } else {
                    throw syntactical_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprLhs:
                if (token.type == Type::op) {
                    context.state = State::DeleteWhereExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprOp:
                if (token.type == Type::literal) {
                    context.deleteStmt->selections.push_back(std::make_pair(Column(),token.value));
                    context.deleteStmt->selections.back().first.name = tokenizer.prev(2).value;
                    context.state = State::DeleteWhereExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprRhs:
                if (token.equalsKeyword(Keyword::And)) {
                    context.state = State::DeleteWhereAnd;
                } else {
                    throw syntactical_error("Expected 'AND', found '" + token.value + "'");
                }
                break;

                //
                //  UPDATE
                //
            case State::Update:
                if (token.type == Type::identifier) {
                    context.updateStmt->relation.name = token.value;
                    context.state = UpdateRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::UpdateRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::UpdateVersion;
                } else if (token.equalsKeyword(Keyword::Set)) {
                    context.updateStmt->relation.version = "master";
                    context.state = State::UpdateSet;
                } else {
                    throw syntactical_error("Expected 'VERSION', 'SET', found '" + token.value + "'");
                }
                break;
            case State::UpdateVersion:
                if (token.hasType(Type::identifier)) {
                    context.updateStmt->relation.version = token.value;
                    context.state = UpdateTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::UpdateTag:
                if (token.equalsKeyword(Keyword::Set)) {
                    context.state = State::UpdateSet;
                } else {
                    throw syntactical_error("Expected 'SET', found '" + token.value + "'");
                }
                break;
            case State::UpdateSet:
            case State::UpdateSetSeperator:
                if (token.type == identifier) {
                    context.state = State::UpdateSetExprLhs;
                } else {
                    throw syntactical_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprLhs:
                if (token.type == Type::op) {
                    context.state = State::UpdateSetExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprOp:
                if (token.type == Type::literal) {
                    context.updateStmt->updates.push_back(std::make_pair(Column(),token.value));
                    context.updateStmt->updates.back().first.name = tokenizer.prev(2).value;
                    context.state = State::UpdateSetExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprRhs:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::UpdateSetSeperator;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::UpdateWhere;
                } else {
                    throw syntactical_error("Expected 'SET' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhere:
            case State::UpdateWhereAnd:
                if (token.type == Type::identifier) {
                    context.state = State::UpdateWhereExprLhs;
                } else {
                    throw syntactical_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprLhs:
                if (token.type == Type::op) {
                    context.state = State::UpdateWhereExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprOp:
                if (token.type == Type::literal) {
                    context.updateStmt->selections.push_back(std::make_pair(Column(),token.value));
                    context.updateStmt->selections.back().first.name = tokenizer.prev(2).value;
                    context.state = State::UpdateWhereExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprRhs:
                if (token.equalsKeyword(Keyword::And)) {
                    context.state = State::UpdateWhereAnd;
                } else {
                    throw syntactical_error("Expected 'AND', found '" + token.value + "'");
                }
                break;

                //
                //  INSERT
                //
            case State::Insert:
                if (token.equalsKeyword(Keyword::Into)) {
                    context.state = State::InsertInto;
                } else {
                    throw syntactical_error("Expected 'INTO', found '" + token.value + "'");
                }
                break;
            case State::InsertInto:
                if (token.type == Type::identifier) {
                    context.insertStmt->relation.name = token.value;
                    context.state = InsertRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::InsertRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = InsertVersion;
                    break;
                }
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = InsertColumnsBegin;
                } else if (token.equalsKeyword(Keyword::Values)) {
                    context.state = InsertValues;
                } else {
                    throw syntactical_error("Expected '(', 'VERSION' or 'VALUES', found '" + token.value + "'");
                }
                context.insertStmt->relation.version = "master";
                break;
            case State::InsertVersion:
                if (token.type == Type::identifier) {
                    context.insertStmt->relation.version = token.value;
                    context.state = InsertTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::InsertTag:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::InsertColumnsBegin;
                } else if (token.equalsKeyword(Keyword::Values)) {
                    context.state = State::InsertValues;
                } else {
                    throw syntactical_error("Expected '(' or 'VALUES', found '" + token.value + "'");
                }
                break;
            case State::InsertColumnsBegin:
            case State::InsertColumnSeperator:
                if (token.type == Type::identifier) {
                    context.insertStmt->columns.push_back(Column());
                    context.insertStmt->columns.back().name = token.value;
                    context.state = InsertColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::InsertColumnName:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = InsertColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = InsertColumnSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;
            case State::InsertColumnsEnd:
                if (token.equalsKeyword(Keyword::Values)) {
                    context.state = InsertValues;
                } else {
                    throw syntactical_error("Expected 'VALUES', found '" + token.value + "'");
                }
                break;
            case State::InsertValues:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = InsertValuesBegin;
                } else {
                    throw syntactical_error("Expected '(', found '" + token.value + "'");
                }
                break;
            case State::InsertValuesBegin:
            case State::InsertValueSeperator:
                if (token.type == Type::literal) {
                    context.insertStmt->values.push_back(token.value);
                    context.state = InsertValue;
                } else {
                    throw syntactical_error("Expected constant, found '" + token.value + "'");
                }
                break;
            case State::InsertValue:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = InsertValuesEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = InsertValueSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;

                //
                //  SELECT
                //
            case State::Select:
            case State::SelectProjectionAttrSeparator:
                if (token.equalsControlSymbol(controlSymbols::star)) {
                    context.state = State::SelectProjectionStar;
                } else if (token.type == Type::identifier) {
                    BindingAttribute columnBinding = parse_binding_attribute(token.value);
                    context.selectStmt->projections.push_back(Column());
                    context.selectStmt->projections.back().name = columnBinding.second;
                    context.selectStmt->projections.back().table = columnBinding.first;
                    context.state = SelectProjectionAttrName;
                } else {
                    throw syntactical_error("Expected column name or star, found '" + token.value + "'");
                }
                break;
            case SelectProjectionStar:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::SelectFrom;
                } else {
                    throw syntactical_error("Expected 'FROM' , found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAttrName:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::SelectFrom;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectProjectionAttrSeparator;
                } else {
                    throw syntactical_error("Expected ',' or 'FROM', found '" + token.value + "'");
                }
                break;
            case SelectFrom:
            case SelectFromSeparator:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.push_back(Table());
                    context.selectStmt->relations.back().name = token.value;
                    context.selectStmt->relations.back().alias = "";
                    context.selectStmt->relations.back().version = "master";
                    context.state = SelectFromRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = SelectFromVersion;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.selectStmt->relations.back().version = "master";

                    context.state = State::SelectFromBindingName;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else {
                    throw syntactical_error("Expected binding name, 'VERSION', 'WHERE' or ',' found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersion:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().version = token.value;
                    context.state = SelectFromTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromTag:
                if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.state = State::SelectFromBindingName;
                } else {
                    throw syntactical_error("Expected binding name , 'WHERE' or ',', found '" + token.value + "'");
                }
                break;
            case State::SelectFromBindingName:
                if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else {
                    throw syntactical_error("Expected ',' or 'Where', found '" + token.value + "'");
                }
                break;
            case State::SelectWhere:
            case State::SelectWhereAnd:
                if (token.type == Type::identifier) {
                    context.state = State::SelectWhereExprLhs;
                } else {
                    throw syntactical_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::SelectWhereExprLhs:
                if (token.type == Type::op) {
                    context.state = State::SelectWhereExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::SelectWhereExprOp:
                if (token.type == Type::identifier) {
                    BindingAttribute lhs = parse_binding_attribute(tokenizer.prev(2).value);
                    BindingAttribute rhs = parse_binding_attribute(token.value);
                    context.selectStmt->joinConditions.push_back(std::make_pair(Column(),Column()));
                    context.selectStmt->joinConditions.back().first.table = lhs.first;
                    context.selectStmt->joinConditions.back().first.name = lhs.second;
                    context.selectStmt->joinConditions.back().second.table = rhs.first;
                    context.selectStmt->joinConditions.back().second.name = rhs.second;
                } else if (token.type == Type::literal) {
                    BindingAttribute lhs = parse_binding_attribute(tokenizer.prev(2).value);
                    context.selectStmt->selections.push_back(std::make_pair(Column(),token.value));
                    context.selectStmt->selections.back().first.table = lhs.first;
                    context.selectStmt->selections.back().first.name = lhs.second;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                context.state = State::SelectWhereExprRhs;
                break;
            case State::SelectWhereExprRhs:
                if (token.equalsKeyword(Keyword::And)) {
                    context.state = State::SelectWhereAnd;
                } else {
                    throw syntactical_error("Expected 'AND', found '" + token.value + "'");
                }
                break;
            default:
                throw syntactical_error("unexpected token: " + token.value);
        }
    }

    void SQLParser::parseStatement(ParsingContext &context, std::string statement) {

        std::stringstream in(statement);
        // Initialize Tokenizer
        Tokenizer tokenizer(in);

        // Parse the input String token by token
        context.state = State::Init;
        while (context.state != State::Done) {
            parseToken(tokenizer, context);
        }
    }
}



// insert into versiontable ( vid , tableid , rid ) values ( '3' , 'table' , '5' ) ;
// update versiontable v set vid = 2 where rid = '2';
// update versiontable v set vid = 2 where rid = '5';
// insert into tasks ( rid , user_id , task_name ) values ( 1 , 1 , 'task1' );
// update tasks t set user_id = 2 where rid = 1;
// update tasks t set task_name = 'task2' where rid = 1;
// create table professoren ( name TEXT , rang NUMERIC );
// create table professoren ( name TEXT NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );
// create table professoren ( name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );
// INSERT INTO professoren ( name , rang ) VALUES ( 'kemper' , 4 );
// INSERT INTO professoren ( name , rang ) VALUES ( 'neumann' , 3 );
// create branch hello from master;
// insert into professoren version hello ( name , rang ) VALUES ( 'neumann' , 3 );
// select name from professoren version hello p;
// DELETE from professoren version hello p WHERE rang = 4;
// DELETE FROM professoren p WHERE rang = 4;

// CREATE TABLE studenten ( matrnr INTEGER NOT NULL , name VARCHAR ( 15 ) NOT NULL , semester INTEGER NOT NULL );
// CREATE TABLE hoeren ( matrnr INTEGER NOT NULL , vorlnr INTEGER NOT NULL );
// INSERT INTO studenten ( matrnr , name , semester ) VALUES ( 1 , 'student1' , 4 );
// INSERT INTO studenten ( matrnr , name , semester ) VALUES ( 2 , 'student2' , 5 );
// INSERT INTO hoeren ( matrnr , vorlnr ) VALUES ( 1 , 5 );
// INSERT INTO hoeren ( matrnr , vorlnr ) VALUES ( 2 , 5 );