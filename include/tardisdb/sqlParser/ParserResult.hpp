//
// Created by Blum Thomas on 2020-06-28.
//

#ifndef PROTODB_PARSERRESULT_HPP
#define PROTODB_PARSERRESULT_HPP

#include <vector>
#include <string>

namespace tardisParser {
    using Relation = std::pair<std::string, std::string>; // relationName and binding
    using BindingAttribute = std::pair<std::string, std::string>; // bindingName and attribute
    using AttributeName = std::string;
    using Constant = std::string;

    struct SQLParserResult {

        enum OpType : unsigned int {
            Select, Insert, Update, Delete, CreateTable, CreateBranch
        } opType;

        std::vector<std::string> versions;

        //SELECT
        std::vector<Relation> relations;
        std::vector<AttributeName> projections;
        std::vector<std::pair<BindingAttribute, Constant>> selections;
        std::vector<std::pair<BindingAttribute, BindingAttribute>> joinConditions;

        //Modification
        std::string relation;

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
}

#endif //PROTODB_PARSERRESULT_HPP
