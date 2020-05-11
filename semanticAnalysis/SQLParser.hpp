#pragma once

#include <vector>
#include <set>
#include <string>
#include "foundations/Database.hpp"

struct incorrect_sql_error : std::runtime_error {
   //semantic or syntactic errors
   using std::runtime_error::runtime_error;
};

struct SQLParserResult {
   using Relation = std::pair<std::string, std::string>; //relationName and binding
   using BindingAttribute = std::pair<std::string, std::string>; //bindingName and attribute
   using AttributeName = std::string;
   using Constant = std::string;
   std::vector<Relation> relations;
   std::vector<AttributeName> projections;
   std::vector<std::pair<BindingAttribute, Constant>> selections;
   std::vector<std::pair<BindingAttribute, BindingAttribute>> joinConditions;
};

SQLParserResult parse_and_analyse_sql_statement(Database& db, std::string sql);
