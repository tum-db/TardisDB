//
// Created by josef on 04.01.17.
//

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/load.hpp"
#include "query_compiler/queryparser.hpp"
#include "query_compiler/semantic_analysis.hpp"

llvm::Function * query_test()
{
    try {
        printf("loading data into tables...\n");
        load();
        printf("done\n");
    } catch (const std::exception & e) {
        fprintf(stderr, "%s\n", e.what());
        throw;
    }

    std::string q1 = "select w_id from warehouse;";
    std::string q2 = "select c_id, c_first, c_middle, c_last from customer where c_last = 'BARBARBAR';";
    std::string q3 = "select d_name from district, order where d_w_id = o_w_id and o_d_id = d_id and o_id = 7;";
    std::string q4 = "select o_id, ol_dist_info from order, orderline where o_id = ol_o_id and ol_d_id = o_d_id and o_w_id = ol_w_id and ol_number = 1 and ol_o_id = 100;";
    std::string q5 = "select o_id, ol_dist_info from orderline, order where o_id = ol_o_id and ol_d_id = o_d_id and o_w_id = ol_w_id and ol_number = 1 and ol_o_id = 100;";
    std::string q6 = "select c_last, o_id, i_id, ol_dist_info from customer, order, orderline, item where c_id = o_c_id and c_d_id = o_d_id and c_w_id = o_w_id and o_id = ol_o_id and ol_d_id = o_d_id and o_w_id = ol_w_id and ol_number = 1 and ol_o_id = 100 and ol_i_id = i_id;";
    std::string q7 = "select c_last, o_id, ol_dist_info from customer, order, orderline where c_id = o_c_id and o_id = ol_o_id and ol_d_id = o_d_id and o_w_id = ol_w_id and ol_number = 1 and ol_o_id = 100;";

    auto parsedQuery = QueryParser::parse_query(q7);
    auto queryTree = computeTree(*parsedQuery.get());

    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    queryTree->produce();

    return funcGen.getFunction();
}
