#include "Parser.hpp"

#include <iostream>
#include <iterator>
#include <algorithm>
#include <cstdlib>

namespace SchemaParser {

namespace keyword {
   const std::string Primary = "primary";
   const std::string Key = "key";
   const std::string Create = "create";
   const std::string Index = "index";
   const std::string On = "on";
   const std::string Table = "table";
   const std::string Integer = "integer";
   const std::string Date = "date";
   const std::string Timestamp = "timestamp";
   const std::string Numeric = "numeric";
   const std::string Not = "not";
   const std::string Null = "null";
   const std::string Char = "char";
   const std::string Varchar = "varchar";
}

namespace literal {
   const char ParenthesisLeft = '(';
   const char ParenthesisRight = ')';
   const char Comma = ',';
   const char Semicolon = ';';
}

std::unique_ptr<Schema> Parser::parse() {
   std::string token;
   unsigned line=1;
   std::unique_ptr<Schema> s(new Schema());
   in.open(fileName.c_str());
   if (!in.is_open())
      throw ParserError(line, "cannot open file '"+fileName+"'");
   while (in >> token) {
      std::string::size_type pos;
      std::string::size_type prevPos = 0;

      size_t c_pos = token.find("--");
      if (c_pos != std::string::npos) {
        if (c_pos != 0) {
          throw "not implemented";
        }
        do {
          if (in.peek() == '\n') {
            ++line;
            break;
          }
        } while (in >> token);
        continue;
      }

      while ((pos = token.find_first_of(",)(;", prevPos)) != std::string::npos) {
         nextToken(line, token.substr(prevPos, pos-prevPos), *s);
         nextToken(line, token.substr(pos,1), *s);
         prevPos=pos+1;
      }
      nextToken(line, token.substr(prevPos), *s);
      while (in.peek() == '\n') {
        in.get();
        ++line;
      }
   }
   in.close();
   return s;
}

static bool isEscaped(const std::string & str) {
   if (str.length() > 2 && str.front() == '"') {
      return (str.back() == '"');
   }
   return false;
}

static std::string unescape(const std::string & str) {
   return str.substr(1, str.size() - 2);
}

static bool isIdentifier(const std::string& str) {
   if (
      str==keyword::Primary ||
      str==keyword::Key ||
      str==keyword::Index ||
      str==keyword::On ||
      str==keyword::Table ||
      str==keyword::Create ||
      str==keyword::Integer ||
      str==keyword::Date ||
      str==keyword::Timestamp ||
      str==keyword::Numeric ||
      str==keyword::Not ||
      str==keyword::Null ||
      str==keyword::Char ||
      str==keyword::Varchar
   )
      return false;

   return str.find_first_not_of("abcdefghijklmnopqrstuvwxyz_1234567890") == std::string::npos;
}

static bool isInt(const std::string& str) {
   return str.find_first_not_of("0123456789") == std::string::npos;
}

void Parser::nextToken(unsigned line, const std::string& token, Schema& schema) {
	if (getenv("DEBUG"))
		std::cerr << line << ": " << token << std::endl;
   if (token.empty())
      return;
   std::string tok;
   std::transform(token.begin(), token.end(), std::back_inserter(tok), tolower);
   switch(state) {
      case State::Semicolon: /* fallthrough */
      case State::Init:
         if (tok==keyword::Create)
            state=State::Create;
         else
            throw ParserError(line, "Expected 'CREATE', found '"+token+"'");
         break;
      case State::Create:
         if (tok==keyword::Table)
            state=State::Table;
         else if (tok==keyword::Index)
            state=State::Index;
         else
            throw ParserError(line, "Expected 'TABLE' or 'INDEX', found '"+token+"'");
         break;
      case State::Index:
         if (isIdentifier(tok)) {
            state=State::IndexName;
            schema.indices.push_back(Schema::SecondaryIndex(token));
         } else {
            throw ParserError(line, "Expected IndexName, found '"+token+"'");
         }
         break;
      case State::IndexName:
         if (tok == keyword::On) {
            state=State::IndexOn;
         } else {
            throw ParserError(line, "Expected 'ON', found '"+token+"'");
         }
         break;
      case State::IndexOn:
         if (isEscaped(tok)) {
            state=State::IndexTableName;
            schema.indices.back().table = unescape(token);
         } else if (isIdentifier(tok)) {
            state=State::IndexTableName;
            schema.indices.back().table = token;
         } else {
            throw ParserError(line, "Expected IndexTableName, found '"+token+"'");
         }
         break;
      case State::IndexTableName:
         if (tok.size()==1 && tok[0]==literal::ParenthesisLeft)
            state=State::IndexKeyListBegin;
         else
            throw ParserError(line, "Expected '(', found '"+token+"'");
         break;
      case State::IndexKeyListBegin:
         if (isIdentifier(tok)) {
            auto & index = schema.indices.back();

            auto it = std::find_if(schema.relations.begin(), schema.relations.end(),
               [&index](const Schema::Relation & rel) -> bool { return (rel.name.compare(index.table) == 0); });
            if (it == schema.relations.end()) {
               throw ParserError(line, "Relation '" + index.table + "' doesn't exist");
            }
            const auto & rel = *it;

            auto attr_it = std::find_if(rel.attributes.begin(), rel.attributes.end(),
               [&token](const std::unique_ptr<Schema::Relation::Attribute> & attr) -> bool
               { return (attr->name.compare(token) == 0); });
            if (attr_it == rel.attributes.end()) {
               throw ParserError(line, "Attribute '" + token + "' doesn't exist");
            }
            index.attributes.push_back(std::distance(rel.attributes.begin(), attr_it));

            // link with table
            if (index.attributes.size() == 1) {
               it->secondaryIndices.push_back(&index);
            }

            state=State::IndexKeyName;
         } else {
            throw ParserError(line, "Expected key attribute, found '"+token+"'");
         }
         break;
      case State::IndexKeyName:
         if (tok.size()==1 && tok[0] == literal::Comma)
            state=State::IndexKeyListBegin;
         else if (tok.size()==1 && tok[0] == literal::ParenthesisRight)
            state=State::IndexEnd;
         else
            throw ParserError(line, "Expected ',' or ')', found '"+token+"'");
         break;
      case State::Table:
         if (isEscaped(tok)) {
            state=State::TableName;
            schema.relations.push_back(Schema::Relation(unescape(token)));
         } else if (isIdentifier(tok)) {
            state=State::TableName;
            schema.relations.push_back(Schema::Relation(token));
         } else {
            throw ParserError(line, "Expected TableName, found '"+token+"'");
         }
         break;
      case State::TableName:
         if (tok.size()==1 && tok[0]==literal::ParenthesisLeft)
            state=State::CreateTableBegin;
         else
            throw ParserError(line, "Expected '(', found '"+token+"'");
         break;
      case State::Separator: /* fallthrough */
      case State::CreateTableBegin:
         if (tok.size()==1 && tok[0]==literal::ParenthesisRight) {
            state=State::CreateTableEnd;
         } else if (tok==keyword::Primary) {
            state=State::Primary;
         } else if (isIdentifier(tok)) {
            auto & rel = schema.relations.back();
            rel.attributes.push_back(std::make_unique<Schema::Relation::Attribute>());
            rel.attributes.back()->name = token;

            if (schema.attributes.count(token) == 0) {
               schema.attributes.insert(
                  std::make_pair(token, rel.attributes.back().get()));
            }

            state=State::AttributeName;
         } else {
            throw ParserError(line, "Expected attribute definition, primary key definition or ')', found '"+token+"'");
         }
         break;
      case State::IndexEnd: /* fallthrough */
      case State::CreateTableEnd:
         if (tok.size()==1 && tok[0]==literal::Semicolon)
            state=State::Semicolon;
         else
            throw ParserError(line, "Expected ';', found '"+token+"'");
         break;
      case State::Primary:
         if (tok==keyword::Key)
            state=State::Key;
         else
            throw ParserError(line, "Expected 'KEY', found '"+token+"'");
         break;
      case State::Key:
         if (tok.size()==1 && tok[0]==literal::ParenthesisLeft)
            state=State::KeyListBegin;
         else
            throw ParserError(line, "Expected list of key attributes, found '"+token+"'");
         break;
      case State::KeyListBegin:
         if (isIdentifier(tok)) {
            struct AttributeNamePredicate {
               const std::string& name;
               AttributeNamePredicate(const std::string& name) : name(name) {}
               bool operator()(const std::unique_ptr<Schema::Relation::Attribute> & attr) const {
                  return attr->name == name;
               }
            };
            const auto& attributes = schema.relations.back().attributes;
            AttributeNamePredicate p(token);
            auto it = std::find_if(attributes.begin(), attributes.end(), p);
            if (it == attributes.end())
               throw ParserError(line, "'"+token+"' is not an attribute of '"+schema.relations.back().name+"'");
            schema.relations.back().primaryKey.push_back(std::distance(attributes.begin(), it));
            state=State::KeyName;
         } else {
            throw ParserError(line, "Expected key attribute, found '"+token+"'");
         }
         break;
      case State::KeyName:
         if (tok.size()==1 && tok[0] == literal::Comma)
            state=State::KeyListBegin;
         else if (tok.size()==1 && tok[0] == literal::ParenthesisRight)
            state=State::KeyListEnd;
         else
            throw ParserError(line, "Expected ',' or ')', found '"+token+"'");
         break;
      case State::KeyListEnd:
         if (tok.size()==1 && tok[0] == literal::Comma)
            state=State::Separator;
         else if (tok.size()==1 && tok[0] == literal::ParenthesisRight)
            state=State::CreateTableEnd;
         else
            throw ParserError(line, "Expected ',' or ')', found '"+token+"'");
         break;
      case State::AttributeName:
         if (tok==keyword::Integer) {
            schema.relations.back().attributes.back()->type=Types::Tag::Integer;
            state=State::AttributeTypeInt;
         } else if (tok==keyword::Date) {
             schema.relations.back().attributes.back()->type=Types::Tag::Date;
             state=State::AttributeTypeDate;
         } else if (tok==keyword::Timestamp) {
            schema.relations.back().attributes.back()->type=Types::Tag::Timestamp;
            state=State::AttributeTypeTimestamp;
         } else if (tok==keyword::Char) {
            schema.relations.back().attributes.back()->type=Types::Tag::Char;
            state=State::AttributeTypeChar;
         } else if (tok==keyword::Varchar) {
            schema.relations.back().attributes.back()->type=Types::Tag::Varchar;
            state=State::AttributeTypeVarchar;
         } else if (tok==keyword::Numeric) {
            schema.relations.back().attributes.back()->type=Types::Tag::Numeric;
            state=State::AttributeTypeNumeric;
         }
         else throw ParserError(line, "Expected type after attribute name, found '"+token+"'");
         break;
      case State::AttributeTypeChar:
         if (tok.size()==1 && tok[0]==literal::ParenthesisLeft)
            state=State::CharBegin;
         else
            throw ParserError(line, "Expected '(' after 'CHAR', found'"+token+"'");
         break;
      case State::CharBegin:
         if (isInt(tok)) {
            schema.relations.back().attributes.back()->len=std::atoi(tok.c_str());
            state=State::CharValue;
         } else {
            throw ParserError(line, "Expected integer after 'CHAR(', found'"+token+"'");
         }
         break;
      case State::CharValue:
         if (tok.size()==1 && tok[0]==literal::ParenthesisRight)
            state=State::CharEnd;
         else
            throw ParserError(line, "Expected ')' after length of CHAR, found'"+token+"'");
         break;
      case State::AttributeTypeVarchar:
         if (tok.size()==1 && tok[0]==literal::ParenthesisLeft)
            state=State::VarcharBegin;
         else
            throw ParserError(line, "Expected '(' after 'VARCHAR', found'"+token+"'");
         break;
      case State::VarcharBegin:
        if (isInt(tok)) {
           schema.relations.back().attributes.back()->len=std::atoi(tok.c_str());
           state=State::VarcharValue;
        } else {
           throw ParserError(line, "Expected integer after 'VARCHAR(', found'"+token+"'");
        }
        break;
      case State::VarcharValue:
        if (tok.size()==1 && tok[0]==literal::ParenthesisRight)
           state=State::VarcharEnd;
        else
           throw ParserError(line, "Expected ')' after length of VARCHAR, found'"+token+"'");
        break;
      case State::AttributeTypeNumeric:
         if (tok.size()==1 && tok[0]==literal::ParenthesisLeft)
            state=State::NumericBegin;
         else
            throw ParserError(line, "Expected '(' after 'NUMERIC', found'"+token+"'");
         break;
      case State::NumericBegin:
         if (isInt(tok)) {
            schema.relations.back().attributes.back()->len=std::atoi(tok.c_str());
            state=State::NumericValue1;
         } else {
            throw ParserError(line, "Expected integer after 'NUMERIC(', found'"+token+"'");
         }
         break;
      case State::NumericValue1:
         if (tok.size()==1 && tok[0]==literal::Comma)
            state=State::NumericSeparator;
         else
            throw ParserError(line, "Expected ',' after first length of NUMERIC, found'"+token+"'");
         break;
      case State::NumericValue2:
         if (tok.size()==1 && tok[0]==literal::ParenthesisRight)
            state=State::NumericEnd;
         else
            throw ParserError(line, "Expected ')' after second length of NUMERIC, found'"+token+"'");
         break;
      case State::NumericSeparator:
         if (isInt(tok)) {
            schema.relations.back().attributes.back()->len2=std::atoi(tok.c_str());
            state=State::NumericValue2;
         } else {
            throw ParserError(line, "Expected second length for NUMERIC type, found'"+token+"'");
         }
         break;
      case State::CharEnd: /* fallthrough */
      case State::VarcharEnd: /* fallthrough */
      case State::NumericEnd: /* fallthrough */
      case State::AttributeTypeDate:
      case State::AttributeTypeTimestamp:
      case State::AttributeTypeInt:
         if (tok.size()==1 && tok[0]==literal::Comma)
            state=State::Separator;
         else if (tok==keyword::Not)
            state=State::Not;
         else if (tok.size()==1 && tok[0]==literal::ParenthesisRight)
				state=State::CreateTableEnd;
         else throw ParserError(line, "Expected ',' or 'NOT NULL' after attribute type, found '"+token+"'");
         break;
      case State::Not:
         if (tok==keyword::Null) {
            schema.relations.back().attributes.back()->notNull=true;
            state=State::Null;
         }
         else throw ParserError(line, "Expected 'NULL' after 'NOT' name, found '"+token+"'");
         break;
      case State::Null:
         if (tok.size()==1 && tok[0]==literal::Comma)
            state=State::Separator;
         else if (tok.size()==1 && tok[0]==literal::ParenthesisRight)
            state=State::CreateTableEnd;
         else throw ParserError(line, "Expected ',' or ')' after attribute definition, found '"+token+"'");
         break;
      default:
         throw;
   }
}

}
