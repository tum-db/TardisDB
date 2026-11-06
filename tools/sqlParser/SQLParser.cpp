#include <algorithm>

#include "sqlParser/SQLParser.hpp"

namespace tardisParser {
    static bool isArithmeticOperator(const Token &token) {
        return (token.hasType(Type::literal) && (token.value == "+" || token.value == "-" || token.value == "/")) ||
               token.equalsControlSymbol(controlSymbols::star);
    }

    static bool equalsIgnoreCase(const std::string &a, const std::string &b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                                  [](char x, char y) { return ::tolower(x) == ::tolower(y); });
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

    /// binding.attribute or attribute
    static Column columnOf(const std::string & value) {
        Column column;
        auto dot = value.find('.');
        column.table = (dot == std::string::npos) ? "" : value.substr(0, dot);
        column.name = (dot == std::string::npos) ? value : value.substr(dot + 1);
        return column;
    }

    static std::string flipped(const std::string & op) {
        if (op == "<") return ">";
        if (op == "<=") return ">=";
        if (op == ">") return "<";
        if (op == ">=") return "<=";
        return op;
    }

    /// Reads ( a , b , ... ) after the opening bracket: identifiers or literals
    static std::vector<Token> readList(Tokenizer & tokenizer) {
        std::vector<Token> items;
        for (;;) {
            Token item = tokenizer.next();
            if (item.type != Type::identifier && item.type != Type::literal) {
                throw syntactical_error("Expected a column or a value, found '" + item.value + "'");
            }
            items.push_back(item);
            Token next = tokenizer.next();
            if (next.equalsControlSymbol(controlSymbols::closeBracket)) return items;
            if (!next.equalsControlSymbol(controlSymbols::separator)) {
                throw syntactical_error("Expected ',' or ')', found '" + next.value + "'");
            }
        }
    }

    /// Parses the conditions of WHERE, beginning with \p first, up to the end of the statement or, if
    /// \p groupMayFollow, to GROUP: a conjunction (AND) of comparisons with =, <>, !=, <, <=, > and >=
    /// of a column and a value (either side), of two row values ( a , b ) >= ( 1 , 2 ), of
    /// column BETWEEN v AND w, of column IN ( v , ... ) and, given \p joins, of two columns (=).
    /// Equalities of a column and a value become selections, all other comparisons predicates.
    /// \returns Whether GROUP ended the conditions
    static bool parseWhere(Tokenizer & tokenizer, Token token, std::vector<std::pair<Column,std::string>> & selections,
                           std::vector<Predicate> & predicates, std::vector<std::pair<Column,Column>> * joins,
                           bool groupMayFollow) {
        auto compare = [&](std::vector<Token> lhs, std::string op, std::vector<Token> rhs) {
            if (op == "!=") op = "<>";
            if (lhs.size() != rhs.size()) throw syntactical_error("row values of different lengths");
            bool lhsColumns = std::all_of(lhs.begin(), lhs.end(), [](const Token & t) { return t.type == Type::identifier; });
            bool rhsColumns = std::all_of(rhs.begin(), rhs.end(), [](const Token & t) { return t.type == Type::identifier; });
            bool rhsValues = std::all_of(rhs.begin(), rhs.end(), [](const Token & t) { return t.type == Type::literal; });
            bool lhsValues = std::all_of(lhs.begin(), lhs.end(), [](const Token & t) { return t.type == Type::literal; });
            if (lhsValues && rhsColumns) {
                std::swap(lhs, rhs);
                op = flipped(op);
                std::swap(lhsColumns, rhsColumns);
                std::swap(lhsValues, rhsValues);
            }
            if (lhsColumns && rhsColumns) {
                if (joins == nullptr || op != "=" || lhs.size() != 1) throw syntactical_error("only = compares two columns");
                joins->emplace_back(columnOf(lhs[0].value), columnOf(rhs[0].value));
                return;
            }
            if (!lhsColumns || !rhsValues) throw syntactical_error("a comparison needs columns on one side and values on the other");
            if (op == "=") {
                for (size_t i = 0; i < lhs.size(); ++i) selections.emplace_back(columnOf(lhs[i].value), rhs[i].value);
                return;
            }
            Predicate predicate;
            for (auto & t : lhs) predicate.columns.push_back(columnOf(t.value));
            predicate.op = op;
            for (auto & t : rhs) predicate.values.push_back(t.value);
            predicates.push_back(std::move(predicate));
        };

        for (;;) {
            // the left side
            std::vector<Token> lhs;
            if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                lhs = readList(tokenizer);
            } else if (token.type == Type::identifier || token.type == Type::literal) {
                lhs.push_back(token);
            } else {
                throw syntactical_error("Expected left expression, found '" + token.value + "'");
            }
            Token op = tokenizer.next();
            std::string word = op.value;
            std::transform(word.begin(), word.end(), word.begin(), ::tolower);
            if (op.type == Type::op) {
                Token rhsToken = tokenizer.next();
                std::vector<Token> rhs;
                if (rhsToken.equalsControlSymbol(controlSymbols::openBracket)) {
                    rhs = readList(tokenizer);
                } else if (rhsToken.type == Type::identifier || rhsToken.type == Type::literal) {
                    rhs.push_back(rhsToken);
                } else {
                    throw syntactical_error("Expected right expression, found '" + rhsToken.value + "'");
                }
                compare(lhs, op.value, rhs);
            } else if (word == "between" && lhs.size() == 1) {
                Token low = tokenizer.next();
                Token andToken = tokenizer.next();
                Token high = tokenizer.next();
                if (low.type != Type::literal || !andToken.equalsKeyword(Keyword::And) || high.type != Type::literal) {
                    throw syntactical_error("Expected BETWEEN value AND value");
                }
                compare(lhs, ">=", {low});
                compare(lhs, "<=", {high});
            } else if (word == "in" && lhs.size() == 1 && lhs[0].type == Type::identifier) {
                Token open = tokenizer.next();
                if (!open.equalsControlSymbol(controlSymbols::openBracket)) throw syntactical_error("Expected '(' after IN");
                Predicate predicate;
                predicate.columns.push_back(columnOf(lhs[0].value));
                predicate.op = "in";
                for (auto & t : readList(tokenizer)) {
                    if (t.type != Type::literal) throw syntactical_error("IN takes values");
                    predicate.values.push_back(t.value);
                }
                predicates.push_back(std::move(predicate));
            } else {
                throw syntactical_error("Expected a comparison, found '" + op.value + "'");
            }

            token = tokenizer.next();
            if (token.hasType(Type::delimiter)) return false;
            if (groupMayFollow && token.equalsKeyword(Keyword::Group)) return true;
            if (!token.equalsKeyword(Keyword::And)) {
                throw syntactical_error("Expected 'AND'" + std::string(groupMayFollow ? ", 'GROUP'" : "") + " or the end, found '" + token.value + "'");
            }
            token = tokenizer.next();
        }
    }

    static bool isWord(const Token & token, const std::string & word) {
        std::string value = token.value;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return (token.type == Type::identifier || token.type == Type::keyword) && value == word;
    }

    /// Reads KEY after PRIMARY of a column: the column is the key and not null
    static void columnPrimaryKey(Tokenizer & tokenizer, CreateTableStatement & stmt) {
        Token key = tokenizer.next();
        if (!isWord(key, "key")) throw syntactical_error("Expected 'KEY', found '" + key.value + "'");
        if (!stmt.primaryKey.empty()) throw syntactical_error("a table has one primary key");
        stmt.primaryKey.push_back(stmt.columns.back().name);
        stmt.columns.back().nullable = false;
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
                } else if (token.equalsKeyword(Keyword::Drop)) {
                    context.state = State::Drop;
                } else if (token.equalsKeyword(Keyword::Merge)) {
                    context.opType = ParsingContext::OpType::MergeBranch;
                    context.mergeBranchStmt = new MergeBranchStatement();
                    context.state = State::Merge;
                } else if (token.equalsKeyword(Keyword::Alter)) {
                    context.state = State::Alter;
                } else {
                    throw syntactical_error("Expected 'Select', 'Insert', 'Update', 'Delete' , 'BRANCH', 'COPY', 'Create', 'DROP', 'MERGE' or 'ALTER', found '" + token.value + "'");
                }
                break;

                //
                //  ALTER TABLE t [ VERSION b ] ADD [ COLUMN ] c type [ NOT NULL ] DEFAULT v
                //  ALTER TABLE t [ VERSION b ] DROP [ COLUMN ] c
                //
            case State::Alter:
                if (token.equalsKeyword(Keyword::Table)) {
                    context.opType = ParsingContext::OpType::AlterTable;
                    context.alterTableStmt = new AlterTableStatement();
                    context.alterTableStmt->relation.version = context.defaultBranch;
                    context.state = State::AlterTable;
                } else {
                    throw syntactical_error("Expected 'TABLE', found '" + token.value + "'");
                }
                break;
            case State::AlterTable:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->relation.name = token.value;
                    context.state = State::AlterTableName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::AlterTableVersion;
                    break;
                }
                // fall through: VERSION is optional
            case State::AlterTableTag:
                if (token.equalsKeyword(Keyword::Add)) {
                    context.alterTableStmt->add = true;
                    context.state = State::AlterTableAdd;
                } else if (token.equalsKeyword(Keyword::Drop)) {
                    context.alterTableStmt->add = false;
                    context.state = State::AlterTableDrop;
                } else {
                    throw syntactical_error("Expected 'VERSION', 'ADD' or 'DROP', found '" + token.value + "'");
                }
                break;
            case State::AlterTableVersion:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->relation.version = token.value;
                    context.state = State::AlterTableTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableAdd:
                if (token.equalsKeyword(Keyword::Column)) {
                    context.state = State::AlterTableAddColumn;
                    break;
                }
                // fall through: COLUMN is optional
            case State::AlterTableAddColumn:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->column.name = token.value;
                    context.state = State::AlterTableColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableColumnName:
                if (token.hasType(Type::identifier)) {
                    auto &column = context.alterTableStmt->column;
                    column.type.clear();
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(column.type), tolower);
                    column.length = 0;
                    column.precision = 0;
                    column.nullable = true;
                    context.state = State::AlterTableColumnType;
                } else {
                    throw syntactical_error("Expected type name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableColumnType:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::AlterTableTypeDetailBegin;
                    break;
                }
                // fall through: the type details are optional
            case State::AlterTableTypeDetailEnd:
                if (token.equalsKeyword(Keyword::Not)) {
                    context.state = State::AlterTableTypeNot;
                } else if (token.equalsKeyword(Keyword::Default)) {
                    context.state = State::AlterTableDefault;
                } else {
                    throw syntactical_error("Expected 'NOT' or 'DEFAULT', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailBegin:
                if (token.hasType(Type::literal) && std::isdigit(token.value[0])) {
                    context.alterTableStmt->column.length = std::stoi(token.value);
                    context.state = State::AlterTableTypeDetailLength;
                } else {
                    throw syntactical_error("Expected type length, found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailLength:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::AlterTableTypeDetailEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::AlterTableTypeDetailSeparator;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailSeparator:
                if (token.hasType(Type::literal) && std::isdigit(token.value[0])) {
                    context.alterTableStmt->column.precision = std::stoi(token.value);
                    context.state = State::AlterTableTypeDetailPrecision;
                } else {
                    throw syntactical_error("Expected type precision, found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailPrecision:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::AlterTableTypeDetailEnd;
                } else {
                    throw syntactical_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeNot:
                if (token.equalsKeyword(Keyword::Null)) {
                    context.alterTableStmt->column.nullable = false;
                    context.state = State::AlterTableTypeNotNull;
                } else {
                    throw syntactical_error("Expected 'NULL', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeNotNull:
                if (token.equalsKeyword(Keyword::Default)) {
                    context.state = State::AlterTableDefault;
                } else {
                    throw syntactical_error("Expected 'DEFAULT', found '" + token.value + "'");
                }
                break;
            case State::AlterTableDefault:
                if (token.hasType(Type::literal) || token.hasType(Type::identifier)) {
                    context.alterTableStmt->defaultValue = token.value;
                    context.alterTableStmt->hasDefault = true;
                    context.state = State::AlterTableDefaultValue;
                } else {
                    throw syntactical_error("Expected default value, found '" + token.value + "'");
                }
                break;
            case State::AlterTableDrop:
                if (token.equalsKeyword(Keyword::Column)) {
                    context.state = State::AlterTableDropColumn;
                    break;
                }
                // fall through: COLUMN is optional
            case State::AlterTableDropColumn:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->column.name = token.value;
                    context.state = State::AlterTableDropName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;

                //
                //  DROP BRANCH b [ CASCADE ]
                //
            case State::Drop:
                if (token.equalsKeyword(Keyword::Branch)) {
                    context.opType = ParsingContext::OpType::DropBranch;
                    context.dropBranchStmt = new DropBranchStatement();
                    context.state = State::DropBranch;
                } else {
                    throw syntactical_error("Expected 'BRANCH', found '" + token.value + "'");
                }
                break;
            case State::DropBranch:
                if (token.hasType(Type::identifier)) {
                    context.dropBranchStmt->branchName = token.value;
                    context.state = State::DropBranchName;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::DropBranchName:
                if (token.equalsKeyword(Keyword::Cascade)) {
                    context.dropBranchStmt->cascade = true;
                    context.state = State::DropBranchCascade;
                } else {
                    throw syntactical_error("Expected 'CASCADE', found '" + token.value + "'");
                }
                break;

                //
                //  MERGE [ BRANCH ] b INTO p
                //
            case State::Merge:
                if (token.equalsKeyword(Keyword::Branch)) {
                    context.state = State::MergeBranch;
                    break;
                }
                // fall through: BRANCH is optional
            case State::MergeBranch:
                if (token.hasType(Type::identifier)) {
                    context.mergeBranchStmt->branchName = token.value;
                    context.state = State::MergeBranchName;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::MergeBranchName:
                if (token.equalsKeyword(Keyword::Into)) {
                    context.state = State::MergeBranchInto;
                } else {
                    throw syntactical_error("Expected 'INTO', found '" + token.value + "'");
                }
                break;
            case State::MergeBranchInto:
                if (token.hasType(Type::identifier)) {
                    context.mergeBranchStmt->targetBranchName = token.value;
                    context.state = State::MergeBranchTarget;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::MergeBranchTarget: {
                // ON CONFLICT KEEP TARGET | KEEP SOURCE
                Token conflict = tokenizer.next();
                Token keep = tokenizer.next();
                Token side = tokenizer.next();
                if (!token.equalsKeyword(Keyword::On) || !isWord(conflict, "conflict") || !isWord(keep, "keep") ||
                    !(isWord(side, "target") || isWord(side, "source")) || !context.mergeBranchStmt->onConflict.empty()) {
                    throw syntactical_error("Expected ON CONFLICT KEEP TARGET or KEEP SOURCE after the target branch");
                }
                context.mergeBranchStmt->onConflict = isWord(side, "target") ? "target" : "source";
                break;
            }

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
                    context.copyStmt->directionFrom = true;
                    context.copyStmt->relation.version = context.defaultBranch;
                    context.state = State::CopyDirection;
                } else if (token.equalsKeyword(Keyword::To)) {
                    context.copyStmt->directionFrom = false;
                    context.copyStmt->relation.version = context.defaultBranch;
                    context.state = State::CopyDirection;
                } else if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::CopyVersion;
                } else {
                    throw syntactical_error("Expected 'FROM','VERSION' OR 'TO', found '" + token.value + "'");
                }
                break;
            case State::CopyVersion:
                if (token.hasType(Type::identifier)) {
                    context.copyStmt->relation.version = token.value;
                    context.state = State::CopyTag;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::CopyTag:
                if (token.equalsKeyword(Keyword::To)) {
                    context.copyStmt->directionFrom = false;
                    context.state = State::CopyDirection;
                } else {
                    throw syntactical_error("Expected 'TO', found '" + token.value + "'");
                }
                break;
            case State::CopyDirection:
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
                } else if (token.equalsKeyword(Keyword::Index)) {
                    context.opType = ParsingContext::OpType::CreateIndex;
                    context.createIndexStmt = new CreateIndexStatement();
                    context.createIndexStmt->relation.version = context.defaultBranch;
                    context.state = State::CreateIndex;
                } else {
                    throw syntactical_error("Expected 'TABLE', 'BRANCH' or 'INDEX', found '" + token.value + "'");
                }
                break;

                //
                //  CREATE INDEX [ name ] ON t ( c ) [ VERSION b ]
                //
            case State::CreateIndex:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->indexName = token.value;
                    context.state = State::CreateIndexName;
                    break;
                }
                // fall through: the name is optional
            case State::CreateIndexName:
                if (token.equalsKeyword(Keyword::On)) {
                    context.state = State::CreateIndexOn;
                } else {
                    throw syntactical_error("Expected index name or 'ON', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexOn:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->relation.name = token.value;
                    context.state = State::CreateIndexTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CreateIndexTable:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::CreateIndexColumnsBegin;
                } else {
                    throw syntactical_error("Expected '(', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexColumnsBegin:
            case State::CreateIndexColumnSeparator:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->columns.push_back(Column());
                    context.createIndexStmt->columns.back().name = token.value;
                    context.state = State::CreateIndexColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateIndexColumnName:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::CreateIndexColumnSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::CreateIndexColumnsEnd;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexColumnsEnd:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::CreateIndexVersion;
                } else {
                    throw syntactical_error("Expected 'VERSION', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexVersion:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->relation.version = token.value;
                    context.state = State::CreateIndexTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
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
                if (isWord(token, "primary")) {
                    auto & stmt = *context.createTableStmt;
                    Token key = tokenizer.next();
                    Token open = tokenizer.next();
                    if (!isWord(key, "key") || !open.equalsControlSymbol(controlSymbols::openBracket)) {
                        throw syntactical_error("Expected 'KEY (' after 'PRIMARY'");
                    }
                    if (!stmt.primaryKey.empty()) throw syntactical_error("a table has one primary key");
                    for (auto & column : readList(tokenizer)) {
                        if (column.type != Type::identifier) throw syntactical_error("Expected column name, found '" + column.value + "'");
                        stmt.primaryKey.push_back(column.value);
                    }
                    Token next = tokenizer.next();
                    if (next.equalsControlSymbol(controlSymbols::closeBracket)) {
                        context.state = CreateTableColumnsEnd;
                    } else if (next.equalsControlSymbol(controlSymbols::separator)) {
                        context.state = CreateTableColumnSeperator;
                    } else {
                        throw syntactical_error("Expected ',' or ')', found '" + next.value + "'");
                    }
                } else if (token.type == Type::identifier) {
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
                if (isWord(token, "primary")) {
                    columnPrimaryKey(tokenizer, *context.createTableStmt);
                    context.state = State::CreateTableTypeNotNull;
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
                if (isWord(token, "primary")) {
                    columnPrimaryKey(tokenizer, *context.createTableStmt);
                    context.state = State::CreateTableTypeNotNull;
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
                if (isWord(token, "primary")) {
                    columnPrimaryKey(tokenizer, *context.createTableStmt);
                    break;
                }
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
                    context.deleteStmt->relation.version = context.defaultBranch;
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
                parseWhere(tokenizer, token, context.deleteStmt->selections, context.deleteStmt->predicates, nullptr, false);
                context.state = State::Done;
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
                    context.updateStmt->relation.version = context.defaultBranch;
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
            case State::UpdateSetExprArith:
                if (token.type == Type::literal || token.type == Type::identifier) {
                    UpdateTerm term;
                    if (context.state == State::UpdateSetExprArith) {
                        term.op = tokenizer.prev(1).value;
                    } else {
                        // the first term; a constant alone is also kept as the value of the update
                        context.updateStmt->updates.push_back(std::make_pair(Column(), token.type == Type::literal ? token.value : ""));
                        context.updateStmt->updates.back().first.name = tokenizer.prev(2).value;
                        context.updateStmt->expressions.emplace_back();
                    }
                    if (token.type == Type::identifier) {
                        BindingAttribute columnBinding = parse_binding_attribute(token.value);
                        term.column.name = columnBinding.second;
                        term.column.table = columnBinding.first;
                    } else {
                        term.value = token.value;
                    }
                    context.updateStmt->expressions.back().push_back(term);
                    context.state = State::UpdateSetExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprRhs:
                if (isArithmeticOperator(token)) {
                    context.state = State::UpdateSetExprArith;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::UpdateSetSeperator;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::UpdateWhere;
                } else {
                    throw syntactical_error("Expected '+', '-', '*', '/', ',' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhere:
                parseWhere(tokenizer, token, context.updateStmt->selections, context.updateStmt->predicates, nullptr, false);
                context.state = State::Done;
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
                context.insertStmt->relation.version = context.defaultBranch;
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
                    auto & stmt = *context.insertStmt;
                    (stmt.rows.empty() ? stmt.values : stmt.rows.back()).push_back(token.value);
                    context.state = InsertValue;
                } else if (token.equalsKeyword(Keyword::Null)) {
                    throw syntactical_error("TardisDB stores no NULL values: versions hold no null indicators");
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
            case State::InsertValuesEnd:
                // VALUES ( ... ) , ( ... ): the next row
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.insertStmt->rows.emplace_back();
                    context.state = InsertValues;
                } else {
                    throw syntactical_error("Expected ',' or the end, found '" + token.value + "'");
                }
                break;

                //
                //  SELECT
                //
            case State::Select:
            case State::SelectProjectionAttrSeparator:
                if (token.equalsControlSymbol(controlSymbols::star)) {
                    context.state = State::SelectProjectionStar;
                } else if (token.equalsKeyword(Keyword::Branch)) {
                    // the branch in which a tuple is visible
                    context.selectStmt->projections.push_back(Column());
                    context.selectStmt->projections.back().name = Keyword::Branch;
                    context.state = SelectProjectionAttrName;
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
                } else if (token.equalsControlSymbol(controlSymbols::openBracket) &&
                           context.selectStmt->projections.back().aggregate.empty() &&
                           context.selectStmt->projections.back().table.empty()) {
                    // the identifier was the name of an aggregate function
                    Column &column = context.selectStmt->projections.back();
                    std::string function;
                    std::transform(column.name.begin(), column.name.end(), std::back_inserter(function), ::tolower);
                    if (function != "count" && function != "sum" && function != "min" && function != "max" && function != "avg") {
                        throw syntactical_error("Unknown aggregate function '" + column.name + "'");
                    }
                    column.aggregate = function;
                    column.name = "";
                    context.state = State::SelectProjectionAggBegin;
                } else {
                    throw syntactical_error("Expected ',' or 'FROM', found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAggBegin:
                if (token.equalsControlSymbol(controlSymbols::star) && context.selectStmt->projections.back().aggregate == "count") {
                    context.selectStmt->projections.back().name = "*";
                    context.state = State::SelectProjectionAggArg;
                } else if (token.type == Type::identifier) {
                    BindingAttribute columnBinding = parse_binding_attribute(token.value);
                    context.selectStmt->projections.back().name = columnBinding.second;
                    context.selectStmt->projections.back().table = columnBinding.first;
                    context.state = State::SelectProjectionAggArg;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAggArg:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::SelectProjectionAttrName;
                } else {
                    throw syntactical_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case SelectFrom:
            case SelectFromSeparator:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.push_back(Table());
                    context.selectStmt->relations.back().name = token.value;
                    context.selectStmt->relations.back().alias = "";
                    context.selectStmt->relations.back().version = context.defaultBranch;
                    context.state = SelectFromRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromRelationName:
                if (token.equalsControlSymbol(controlSymbols::openBracket) &&
                    equalsIgnoreCase(context.selectStmt->relations.back().name, "diff")) {
                    // DIFF ( t VERSION a , t VERSION b )
                    context.selectStmt->relations.back().name = "";
                    context.selectStmt->relations.back().versionScope = "diff";
                    context.state = SelectFromDiffBegin;
                } else if (token.equalsKeyword(Keyword::Version)) {
                    context.state = SelectFromVersion;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.selectStmt->relations.back().version = context.defaultBranch;

                    context.state = State::SelectFromBindingName;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
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
                } else if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    // VERSION ( b1 , b2 , ... )
                    context.selectStmt->relations.back().versionScope = "set";
                    context.state = SelectFromVersionSetBegin;
                } else if (token.equalsKeyword(Keyword::All)) {
                    // VERSION ALL [ DESCENDANTS | LEAVES OF b ]
                    context.selectStmt->relations.back().versionScope = "all";
                    context.state = SelectFromVersionAll;
                } else {
                    throw syntactical_error("Expected version name, '(' or 'ALL', found '" + token.value + "'");
                }
                break;
            case State::SelectFromDiffBegin:
            case State::SelectFromDiffSeparator:
                if (token.type == Type::identifier) {
                    Table &relation = context.selectStmt->relations.back();
                    if (relation.name.empty()) {
                        relation.name = token.value;
                    } else if (relation.name != token.value) {
                        throw syntactical_error("DIFF compares two versions of one table, found '" + relation.name + "' and '" + token.value + "'");
                    }
                    context.state = SelectFromDiffTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromDiffTable:
            case State::SelectFromDiffName: {
                Table &relation = context.selectStmt->relations.back();
                if (context.state == State::SelectFromDiffTable && token.equalsKeyword(Keyword::Version)) {
                    context.state = SelectFromDiffVersion;
                    break;
                }
                if (context.state == State::SelectFromDiffTable) {
                    relation.versionSet.push_back(context.defaultBranch);
                }
                if (token.equalsControlSymbol(controlSymbols::separator) && relation.versionSet.size() == 1) {
                    context.state = SelectFromDiffSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::closeBracket) && relation.versionSet.size() == 2) {
                    context.state = SelectFromTag;
                } else {
                    throw syntactical_error(std::string("Expected ") + (relation.versionSet.size() == 1 ? "','" : "')'") + ", found '" + token.value + "'");
                }
                break;
            }
            case State::SelectFromDiffVersion:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().versionSet.push_back(token.value);
                    context.state = SelectFromDiffName;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionSetBegin:
            case State::SelectFromVersionSetSeparator:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().versionSet.push_back(token.value);
                    context.state = SelectFromVersionSetName;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionSetName:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = SelectFromVersionSetSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = SelectFromTag;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionAll:
                if (token.equalsKeyword(Keyword::Descendants) || token.equalsKeyword(Keyword::Leaves)) {
                    std::string scope;
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(scope), ::tolower);
                    context.selectStmt->relations.back().versionScope = scope;
                    context.state = SelectFromVersionAllScope;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.state = State::SelectFromBindingName;
                } else {
                    throw syntactical_error("Expected 'DESCENDANTS', 'LEAVES', binding name, 'WHERE' or 'GROUP', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionAllScope:
                if (token.equalsKeyword(Keyword::Of)) {
                    context.state = SelectFromVersionAllOf;
                } else {
                    throw syntactical_error("Expected 'OF', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionAllOf:
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
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
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
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else {
                    throw syntactical_error("Expected ',' or 'Where', found '" + token.value + "'");
                }
                break;
            case State::SelectWhere:
                if (parseWhere(tokenizer, token, context.selectStmt->selections, context.selectStmt->predicates,
                               &context.selectStmt->joinConditions, true)) {
                    context.state = State::SelectGroup;
                } else {
                    context.state = State::Done;
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
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else {
                    throw syntactical_error("Expected 'AND' or 'GROUP', found '" + token.value + "'");
                }
                break;
            case State::SelectGroup:
                if (token.equalsKeyword(Keyword::By)) {
                    context.state = State::SelectGroupBy;
                } else {
                    throw syntactical_error("Expected 'BY', found '" + token.value + "'");
                }
                break;
            case State::SelectGroupBy:
            case State::SelectGroupBySeparator:
                if (token.equalsKeyword(Keyword::Branch)) {
                    context.selectStmt->groupBy.push_back(Column());
                    context.selectStmt->groupBy.back().name = Keyword::Branch;
                    context.state = State::SelectGroupByName;
                } else if (token.type == Type::identifier) {
                    BindingAttribute columnBinding = parse_binding_attribute(token.value);
                    context.selectStmt->groupBy.push_back(Column());
                    context.selectStmt->groupBy.back().name = columnBinding.second;
                    context.selectStmt->groupBy.back().table = columnBinding.first;
                    context.state = State::SelectGroupByName;
                } else {
                    throw syntactical_error("Expected 'BRANCH' or column name, found '" + token.value + "'");
                }
                break;
            case State::SelectGroupByName:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectGroupBySeparator;
                } else {
                    throw syntactical_error("Expected ',', found '" + token.value + "'");
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