#include <algorithm>

#include "sqlParser/SQLParser.hpp"

namespace tardisParser {
    // Check if token matches a certain keyword
    bool SQLParser::equals_keyword(const Token &tok, std::string keyword) {
        return (tok.type == TokenType::keyword && tok.value.compare(keyword) == 0);
    }

    // Check if token matches a certain control symbol
    bool SQLParser::equals_controlSymbol(const Token &tok, std::string controlSymbol) {
        return (tok.type == TokenType::controlSymbol && tok.value.compare(controlSymbol) == 0);
    }

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

    state_t SQLParser::parse_next_token(Tokenizer &token_src, const state_t state, SQLParserResult &query) {
        // Get the next token from the tokenizer
        const Token &token = token_src.next();

        // When the token is a delimiter check if the parser is in a valid final state
        if (token.type == TokenType::delimiter) {
            if (state != State::SelectFromBindingName &&
                state != State::SelectWhereExprRhs &&
                state != State::InsertValuesEnd &&
                state != State::UpdateSetExprRhs &&
                state != State::UpdateWhereExprRhs &&
                state != State::DeleteRelationName &&
                state != State::DeleteWhereExprRhs &&
                state != State::CreateTableRelationName &&
                state != State::CreateTableColumnsEnd &&
                state != State::CreateBranchParent) {
                throw incorrect_sql_error("unexpected end of input");
            }
            return State::Done;
        }

        // Implements state transitions
        state_t new_state;
        switch (state) {
            case State::Init:
                if (equals_keyword(token,keywords::Select)) {
                    query.opType = SQLParserResult::OpType::Select;
                    new_state = State::Select;
                } else if (equals_keyword(token,keywords::Insert)) {
                    query.opType = SQLParserResult::OpType::Insert;
                    new_state = State::Insert;
                } else if (equals_keyword(token,keywords::Update)) {
                    query.opType = SQLParserResult::OpType::Update;
                    new_state = State::Update;
                } else if (equals_keyword(token,keywords::Delete)) {
                    query.opType = SQLParserResult::OpType::Delete;
                    new_state = State::Delete;
                } else if (equals_keyword(token,keywords::Create)) {
                    new_state = State::Create;
                } else {
                    throw incorrect_sql_error("Expected 'Select', 'Insert', 'Update', 'Delete' or 'Create', found '" + token.value + "'");
                }
                break;

                //
                //  CREATE
                //
            case State::Create:
                if (equals_keyword(token, keywords::Table)) {
                    query.opType = SQLParserResult::OpType::CreateTable;
                    new_state = State::CreateTable;
                } else if (equals_keyword(token, keywords::Branch)) {
                    query.opType = SQLParserResult::OpType::CreateBranch;
                    new_state = State::CreateBranch;
                } else {
                    throw incorrect_sql_error("Expected 'TABLE' or 'Branch', found '" + token.value + "'");
                }
                break;
            case State::CreateBranch:
                if (token.type == TokenType::identifier) {
                    query.branchId = token.value;
                    new_state = State::CreateBranchTag;
                } else {
                    throw incorrect_sql_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::CreateBranchTag:
                if (equals_keyword(token,keywords::From)) {
                    new_state = State::CreateBranchFrom;
                } else {
                    throw incorrect_sql_error("Expected 'FROM', found '" + token.value + "'");
                }
                break;
            case State::CreateBranchFrom:
                if (token.type == TokenType::identifier) {
                    query.parentBranchId = token.value;
                    new_state = State::CreateBranchParent;
                } else {
                    throw incorrect_sql_error("Expected parent branch name, found '" + token.value + "'");
                }
                break;

            case State::CreateTable:
                if (token.type == TokenType::identifier) {
                    query.relation = token.value;
                    new_state = State::CreateTableRelationName;
                } else {
                    throw incorrect_sql_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableRelationName:
                if (equals_controlSymbol(token,controlSymbols::openBracket)) {
                    new_state = State::CreateTableColumnsBegin;
                }
                break;
            case State::CreateTableColumnsBegin:
            case State::CreateTableColumnSeperator:
                if (token.type == TokenType::identifier) {
                    query.columnNames.emplace_back(token.value);
                    new_state = CreateTableColumnName;
                } else {
                    throw incorrect_sql_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableColumnName:
                if (token.type == TokenType::identifier) {
                    std::string lowercase_token_value;
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(lowercase_token_value), tolower);
                    query.columnTypes.emplace_back(lowercase_token_value);
                    new_state = CreateTableColumnType;
                } else {
                    throw incorrect_sql_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableColumnType:
                if (equals_controlSymbol(token,controlSymbols::openBracket)) {
                    new_state = CreateTableTypeDetailBegin;
                    break;
                }
                query.length.emplace_back(0);
                query.precision.emplace_back(0);
                if (equals_keyword(token,keywords::Not)) {
                    new_state = CreateTableTypeNot;
                    break;
                }
                query.nullable.emplace_back(true);

                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    new_state = CreateTableColumnsEnd;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = CreateTableColumnSeperator;
                } else {
                    throw incorrect_sql_error("Expected '(' , ',' , ')' or 'NOT', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailBegin:
                if (token.type == literal && std::isdigit(token.value[0])) {
                    query.length.emplace_back(std::stoi(token.value));
                    new_state = CreateTableTypeDetailLength;
                } else {
                    throw incorrect_sql_error("Expected type length, found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailLength:
                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    query.precision.emplace_back(0);
                    new_state = CreateTableTypeDetailEnd;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = CreateTableTypeDetailSeperator;
                } else {
                    throw incorrect_sql_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailSeperator:
                if (token.type == literal && std::isdigit(token.value[0])) {
                    query.precision.emplace_back(std::stoi(token.value));
                    new_state = CreateTableTypeDetailPrecision;
                } else {
                    throw incorrect_sql_error("Expected type precision, found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailPrecision:
                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    new_state = CreateTableTypeDetailEnd;
                } else {
                    throw incorrect_sql_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailEnd:
                if (equals_keyword(token,keywords::Not)) {
                    new_state = CreateTableTypeNot;
                    break;
                }
                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    new_state = CreateTableColumnsEnd;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = CreateTableColumnSeperator;
                } else {
                    throw incorrect_sql_error("Expected ',' , ')' or 'NOT', found '" + token.value + "'");
                }
                query.nullable.emplace_back(true);
                break;
            case State::CreateTableTypeNot:
                if (equals_keyword(token,keywords::Null)) {
                    query.nullable.emplace_back(false);
                    new_state = State::CreateTableTypeNotNull;
                } else {
                    throw incorrect_sql_error("Expected 'NULL', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeNotNull:
                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    new_state = CreateTableColumnsEnd;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = CreateTableColumnSeperator;
                } else {
                    throw incorrect_sql_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;

                //
                //  DELETE
                //
            case State::Delete:
                if (equals_keyword(token,keywords::From)) {
                    new_state = State::DeleteFrom;
                } else {
                    throw incorrect_sql_error("Expected 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteFrom:
                if (token.type == TokenType::identifier) {
                    using rel_t = decltype(query.relations)::value_type;
                    query.relations.push_back(rel_t(token.value, {}));
                    new_state = DeleteRelationName;
                } else {
                    throw incorrect_sql_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::DeleteRelationName:
                if (equals_keyword(token, keywords::Version)) {
                    new_state = State::DeleteVersion;
                } else if (equals_keyword(token, keywords::Where)) {
                    query.versions.emplace_back("master");
                    new_state = State::DeleteWhere;
                } else {
                    throw incorrect_sql_error("Expected 'VERSION' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteVersion:
                if (token.type == TokenType::identifier) {
                    query.versions.emplace_back(token.value);
                    new_state = DeleteTag;
                } else {
                    throw incorrect_sql_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::DeleteTag:
                if (equals_keyword(token, keywords::Where)) {
                    new_state = State::DeleteWhere;
                } else {
                    throw incorrect_sql_error("Expected 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteWhere:
            case State::DeleteWhereAnd:
                if (token.type == TokenType::identifier) {
                    new_state = State::DeleteWhereExprLhs;
                } else {
                    throw incorrect_sql_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprLhs:
                if (token.type == TokenType::op) {
                    new_state = State::DeleteWhereExprOp;
                } else {
                    throw incorrect_sql_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprOp:
                if (token.type == TokenType::literal) {
                    std::string lhs = token_src.prev(2).value;
                    Constant rhs = token.value;
                    query.selections.push_back(std::make_pair(BindingAttribute("", lhs), rhs));
                    new_state = State::DeleteWhereExprRhs;
                } else {
                    throw incorrect_sql_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprRhs:
                if (equals_keyword(token, keywords::And)) {
                    new_state = State::DeleteWhereAnd;
                } else {
                    throw incorrect_sql_error("Expected 'AND', found '" + token.value + "'");
                }
                break;

                //
                //  UPDATE
                //
            case State::Update:
                if (token.type == TokenType::identifier) {
                    using rel_t = decltype(query.relations)::value_type;
                    query.relations.push_back(rel_t(token.value, {}));
                    new_state = UpdateRelationName;
                } else {
                    throw incorrect_sql_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::UpdateRelationName:
                if (equals_keyword(token,keywords::Version)) {
                    new_state = State::UpdateVersion;
                } else if (equals_keyword(token,keywords::Set)) {
                    query.versions.emplace_back("master");
                    new_state = State::UpdateSet;
                } else {
                    throw incorrect_sql_error("Expected 'VERSION', 'SET', found '" + token.value + "'");
                }
                break;
            case State::UpdateVersion:
                if (token.type == TokenType::identifier) {
                    query.versions.emplace_back(token.value);
                    new_state = UpdateTag;
                } else {
                    throw incorrect_sql_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::UpdateTag:
                if (equals_keyword(token,keywords::Set)) {
                    new_state = State::UpdateSet;
                } else {
                    throw incorrect_sql_error("Expected 'SET', found '" + token.value + "'");
                }
                break;
            case State::UpdateSet:
            case State::UpdateSetSeperator:
                if (token.type == identifier) {
                    new_state = State::UpdateSetExprLhs;
                } else {
                    throw incorrect_sql_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprLhs:
                if (token.type == TokenType::op) {
                    new_state = State::UpdateSetExprOp;
                } else {
                    throw incorrect_sql_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprOp:
                if (token.type == TokenType::literal) {
                    std::string lhs = token_src.prev(2).value;
                    query.columnToValue.push_back(std::make_pair(lhs, token.value));
                    new_state = State::UpdateSetExprRhs;
                } else {
                    throw incorrect_sql_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprRhs:
                if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = State::UpdateSetSeperator;
                } else if (equals_keyword(token,keywords::Where)) {
                    new_state = State::UpdateWhere;
                } else {
                    throw incorrect_sql_error("Expected 'SET' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhere:
            case State::UpdateWhereAnd:
                if (token.type == TokenType::identifier) {
                    new_state = State::UpdateWhereExprLhs;
                } else {
                    throw incorrect_sql_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprLhs:
                if (token.type == TokenType::op) {
                    new_state = State::UpdateWhereExprOp;
                } else {
                    throw incorrect_sql_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprOp:
                if (token.type == TokenType::literal) {
                    std::string lhs = token_src.prev(2).value;
                    Constant rhs = token.value;
                    query.selections.push_back(std::make_pair(BindingAttribute("", lhs), rhs));
                    new_state = State::UpdateWhereExprRhs;
                } else {
                    throw incorrect_sql_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprRhs:
                if (equals_keyword(token, keywords::And)) {
                    new_state = State::UpdateWhereAnd;
                } else {
                    throw incorrect_sql_error("Expected 'AND', found '" + token.value + "'");
                }
                break;

                //
                //  INSERT
                //
            case State::Insert:
                if (equals_keyword(token, keywords::Into)) {
                    new_state = State::InsertInto;
                } else {
                    throw incorrect_sql_error("Expected 'INTO', found '" + token.value + "'");
                }
                break;
            case State::InsertInto:
                if (token.type == TokenType::identifier) {
                    query.relation = token.value;
                    new_state = InsertRelationName;
                } else {
                    throw incorrect_sql_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::InsertRelationName:
                if (equals_keyword(token,keywords::Version)) {
                    new_state = InsertVersion;
                    break;
                }
                if (equals_controlSymbol(token,controlSymbols::openBracket)) {
                    new_state = InsertColumnsBegin;
                } else if (equals_keyword(token,keywords::Values)) {
                    new_state = InsertValues;
                } else {
                    throw incorrect_sql_error("Expected '(', 'VERSION' or 'VALUES', found '" + token.value + "'");
                }
                query.versions.emplace_back("master");
                break;
            case State::InsertVersion:
                if (token.type == TokenType::identifier) {
                    query.versions.emplace_back(token.value);
                    new_state = InsertTag;
                } else {
                    throw incorrect_sql_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::InsertTag:
                if (equals_controlSymbol(token,controlSymbols::openBracket)) {
                    new_state = State::InsertColumnsBegin;
                } else if (equals_keyword(token,keywords::Values)) {
                    new_state = State::InsertValues;
                } else {
                    throw incorrect_sql_error("Expected '(' or 'VALUES', found '" + token.value + "'");
                }
                break;
            case State::InsertColumnsBegin:
            case State::InsertColumnSeperator:
                if (token.type == TokenType::identifier) {
                    query.columnNames.emplace_back(token.value);
                    new_state = InsertColumnName;
                } else {
                    throw incorrect_sql_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::InsertColumnName:
                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    new_state = InsertColumnsEnd;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = InsertColumnSeperator;
                } else {
                    throw incorrect_sql_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;
            case State::InsertColumnsEnd:
                if (equals_keyword(token,keywords::Values)) {
                    new_state = InsertValues;
                } else {
                    throw incorrect_sql_error("Expected 'VALUES', found '" + token.value + "'");
                }
                break;
            case State::InsertValues:
                if (equals_controlSymbol(token,controlSymbols::openBracket)) {
                    new_state = InsertValuesBegin;
                } else {
                    throw incorrect_sql_error("Expected '(', found '" + token.value + "'");
                }
                break;
            case State::InsertValuesBegin:
            case State::InsertValueSeperator:
                if (token.type == TokenType::literal) {
                    query.values.emplace_back(token.value);
                    new_state = InsertValue;
                } else {
                    throw incorrect_sql_error("Expected constant, found '" + token.value + "'");
                }
                break;
            case State::InsertValue:
                if (equals_controlSymbol(token,controlSymbols::closeBracket)) {
                    new_state = InsertValuesEnd;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = InsertValueSeperator;
                } else {
                    throw incorrect_sql_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;

                //
                //  SELECT
                //
            case State::Select:
            case State::SelectProjectionAttrSeparator:
                if (equals_controlSymbol(token,controlSymbols::star)) {
                    new_state = State::SelectProjectionStar;
                } else if (token.type == TokenType::identifier) {
                    query.projections.push_back(token.value);
                    new_state = SelectProjectionAttrName;
                } else {
                    throw incorrect_sql_error("Expected column name or star, found '" + token.value + "'");
                }
                break;
            case SelectProjectionStar:
                if (equals_keyword(token,keywords::From)) {
                    new_state = State::SelectFrom;
                } else {
                    throw incorrect_sql_error("Expected 'FROM' , found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAttrName:
                if (equals_keyword(token,keywords::From)) {
                    new_state = State::SelectFrom;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = State::SelectProjectionAttrSeparator;
                } else {
                    throw incorrect_sql_error("Expected ',' or 'FROM', found '" + token.value + "'");
                }
                break;
            case SelectFrom:
            case SelectFromSeparator:
                if (token.type == TokenType::identifier) {
                    using rel_t = decltype(query.relations)::value_type;
                    query.relations.push_back(rel_t(token.value, {}));
                    new_state = SelectFromRelationName;
                } else {
                    throw incorrect_sql_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromRelationName:
                if (equals_keyword(token,keywords::Version)) {
                    new_state = SelectFromVersion;
                } else if (token.type == TokenType::identifier) {
                    using rel_t = decltype(query.relations)::value_type;
                    auto current = query.relations.back();
                    query.relations.pop_back();
                    query.relations.push_back(rel_t(current.first, token.value));

                    query.versions.emplace_back("master");

                    new_state = State::SelectFromBindingName;
                } else {
                    throw incorrect_sql_error("Expected binding name or 'VERSION', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersion:
                if (token.type == TokenType::identifier) {
                    query.versions.emplace_back(token.value);
                    new_state = SelectFromTag;
                } else {
                    throw incorrect_sql_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromTag:
                if (equals_keyword(token, keywords::Where)) {
                    new_state = State::SelectWhere;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = State::SelectFromSeparator;
                } else if (token.type == TokenType::identifier) {
                    using rel_t = decltype(query.relations)::value_type;
                    auto current = query.relations.back();
                    query.relations.pop_back();
                    query.relations.push_back(rel_t(current.first, token.value));

                    new_state = State::SelectFromBindingName;
                } else {
                    throw incorrect_sql_error("Expected binding name , 'WHERE' or ',', found '" + token.value + "'");
                }
                break;
            case State::SelectFromBindingName:
                if (equals_keyword(token, keywords::Where)) {
                    new_state = State::SelectWhere;
                } else if (equals_controlSymbol(token,controlSymbols::separator)) {
                    new_state = State::SelectFromSeparator;
                } else {
                    throw incorrect_sql_error("Expected ',' or 'Where', found '" + token.value + "'");
                }
                break;
            case State::SelectWhere:
            case State::SelectWhereAnd:
                if (token.type == TokenType::identifier) {
                    new_state = State::SelectWhereExprLhs;
                } else {
                    throw incorrect_sql_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::SelectWhereExprLhs:
                if (token.type == TokenType::op) {
                    new_state = State::SelectWhereExprOp;
                } else {
                    throw incorrect_sql_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::SelectWhereExprOp:
                if (token.type == TokenType::identifier) {
                    BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
                    BindingAttribute rhs = parse_binding_attribute(token.value);
                    query.joinConditions.push_back(std::make_pair(lhs, rhs));
                } else if (token.type == TokenType::literal) {
                    BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
                    query.selections.push_back(std::make_pair(lhs, token.value));
                } else {
                    throw incorrect_sql_error("Expected right expression, found '" + token.value + "'");
                }
                new_state = State::SelectWhereExprRhs;
                break;
            case State::SelectWhereExprRhs:
                if (equals_keyword(token, keywords::And)) {
                    new_state = State::SelectWhereAnd;
                } else {
                    throw incorrect_sql_error("Expected 'AND', found '" + token.value + "'");
                }
                break;
            default:
                throw incorrect_sql_error("unexpected token: " + token.value);
        }

        return new_state;
    }

    SQLParserResult SQLParser::parse_sql_statement(std::string sql) {
        SQLParserResult result;

        std::stringstream in(sql);
        // Initialize Tokenizer
        Tokenizer tokenizer(in);

        // Parse the input String token by token
        state_t state = State::Init;
        while (state != State::Done) {
            state = parse_next_token(tokenizer, state, result);
        }

        return result;
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