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

   std::string opType;

   //SELECT
   std::vector<Relation> relations;
   std::vector<AttributeName> projections;
   std::vector<std::pair<BindingAttribute, Constant>> selections;
   std::vector<std::pair<BindingAttribute, BindingAttribute>> joinConditions;

   //Modification
   std::string relation;
   std::vector<std::pair<std::string, std::string>> selectionsWithoutBinding;

   //INSERT
   std::vector<std::string> columnNames;
   std::vector<std::string> values;

   //UPDATE
   std::vector<std::pair<std::string,std::string>> columnToValue;

   //CREATE
   std::vector<std::string> columnTypes;
   std::vector<bool> nullable;
   std::vector<uint32_t> length;
   std::vector<uint32_t> precision;

   //Checkout
   std::string branchId;
   std::string parentBranchId;
};

SQLParserResult parse_and_analyse_sql_statement(Database& db, std::string sql);
