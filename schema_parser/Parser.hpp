#ifndef H_Parser_hpp
#define H_Parser_hpp

#include <exception>
#include <string>
#include <memory>
#include <fstream>
#include <sstream>
#include "Schema.hpp"

namespace SchemaParser {

class ParserError : std::exception {
   std::string msg;
   unsigned line;
   public:
   ParserError(unsigned line, const std::string& m) : msg(m), line(line) {}
   ~ParserError() throw() {}
   const char* what() const throw() {
      return msg.c_str();
   }
   unsigned get_line() const throw() {
      return line;
   }
};

struct Parser {
   std::string fileName;
   std::ifstream in;
   enum class State : unsigned { Init, Create, Index, IndexName, IndexOn, IndexTableName, IndexKeyListBegin, IndexKeyName, IndexEnd, Table, CreateTableBegin, CreateTableEnd, TableName, Primary, Key, KeyListBegin, KeyName, KeyListEnd, AttributeName, AttributeTypeInt, AttributeTypeDate, AttributeTypeTimestamp, AttributeTypeChar, CharBegin, CharValue, CharEnd, AttributeTypeVarchar, VarcharBegin, VarcharValue, VarcharEnd, AttributeTypeNumeric, NumericBegin, NumericValue1, NumericSeparator, NumericValue2, NumericEnd, Not, Null, Separator, Semicolon};
   State state;
   Parser(const std::string& fileName) : fileName(fileName), state(State::Init) {}
   ~Parser() {};
   std::unique_ptr<SchemaParser::Schema> parse();

   private:
   void nextToken(unsigned line, const std::string& token, SchemaParser::Schema& s);
};

}

#endif
