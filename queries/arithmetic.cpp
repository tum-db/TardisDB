
#include <random>
#include <cmath>

#include <llvm/IR/TypeBuilder.h>

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "query_compiler/compiler.hpp"
#include "queries/common.hpp"

using namespace Algebra::Logical;
using namespace Algebra::Logical::Aggregations;
using namespace Algebra::Logical::Expressions;

/*
query1:

select
        l_returnflag,
        l_linestatus,
        sum(l_discount^n as numeric(17,4))
from
        lineitem
group by
        l_returnflag,
        l_linestatus

query2:
select
        l_returnflag,
        l_linestatus,
        sum(c1*l_discount^1 + c2*l_discount^2 + ... + cn*l_discount^n as numeric(17,4))
from
        lineitem
group by
        l_returnflag,
        l_linestatus

query3:
select
        l_returnflag,
        l_linestatus,
        min(l_linenumber^n)
from
        lineitem
group by
        l_returnflag,
        l_linestatus

query4:
select
        l_returnflag,
        l_linestatus,
        min(c1*l_linenumber^1 + c2*l_linenumber^2 + ... + cn*l_linenumber^n)
from
        lineitem
group by
        l_returnflag,
        l_linestatus
*/

static llvm::Function * gen_arithmentic1_func(Database & db)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    QueryContext context(db);
    std::vector<iu_p_t> selection;

    Table * lineitem = db.getTable("lineitem");
    assert(lineitem != nullptr);
    auto scan = std::make_unique<TableScan>( context, *lineitem );
    addToScope(context, *scan);

    // collect ius
    iu_p_t l_returnflagIU    = lookup(context, "l_returnflag");
    iu_p_t l_linestatusIU    = lookup(context, "l_linestatus");
    iu_p_t l_discountIU      = lookup(context, "l_discount");

    // keep l_returnflag
    auto l_returnflagKeep = std::make_unique<Aggregations::Keep>( context, l_returnflagIU );
    selection.push_back( l_returnflagKeep->getProduced() );

    // keep l_linestatus
    auto l_linestatusKeep = std::make_unique<Aggregations::Keep>( context, l_linestatusIU );
    selection.push_back( l_linestatusKeep->getProduced() );

    // build a polynomial
    const unsigned rank = 3; // min 1

    Sql::SqlType resultTy = Sql::getNumericFullLengthTy(4, true);

    // Horner's methode
    std::unique_ptr<Expression> polynomial;
    for (unsigned i = 0; i < rank; ++i) {
        auto x = std::make_unique<Identifier>(l_discountIU);

        if (polynomial) {
            polynomial = std::make_unique<Multiplication>(
                std::move(polynomial),
                std::move(x)
            );

            // cast
            polynomial = std::make_unique<Cast>( std::move(polynomial), resultTy );
        } else {
            polynomial = std::move(x);
        }
    }

    auto aggr = std::make_unique<Aggregations::Sum>( context, std::move(polynomial) );
    selection.push_back( aggr->getProduced() );

    std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
    aggregations.push_back(std::move(l_returnflagKeep));
    aggregations.push_back(std::move(l_linestatusKeep));
    aggregations.push_back(std::move(aggr));

    auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

    auto result = std::make_unique<Result>( std::move(group), selection );
    verifyDependencies(*result);

    auto physicalTree = Algebra::translateToPhysicalTree(*result);
    physicalTree->produce();

    return funcGen.getFunction();
}

/*
void benchmark_arithmetic_1(unsigned runs)
{
    auto db = load_tpch_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic1_func(*db);

    auto times = QueryCompiler::benchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}

void run_arithmetic_1()
{
    auto db = load_tpch_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic1_func(*db);
    QueryCompiler::execute(queryFunc);
}
*/
void benchmark_arithmetic_1_null(unsigned runs)
{
    auto db = load_tpch_null_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic1_func(*db);

    auto times = QueryCompiler::compileAndBenchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}
/*
void run_arithmetic_1_null()
{
    auto db = load_tpch_null_db();

    printf("running query...\n");
    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic1_func(*db);
    QueryCompiler::execute(queryFunc);
}
*/

static llvm::Function * gen_arithmentic2_func(Database & db)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    QueryContext context(db);
    std::vector<iu_p_t> selection;

    Table * lineitem = db.getTable("lineitem");
    assert(lineitem != nullptr);
    auto scan = std::make_unique<TableScan>( context, *lineitem );
    addToScope(context, *scan);

    // collect ius
    iu_p_t l_returnflagIU    = lookup(context, "l_returnflag");
    iu_p_t l_linestatusIU    = lookup(context, "l_linestatus");
    iu_p_t l_discountIU      = lookup(context, "l_discount");

    // keep l_returnflag
    auto l_returnflagKeep = std::make_unique<Aggregations::Keep>( context, l_returnflagIU );
    selection.push_back( l_returnflagKeep->getProduced() );

    // keep l_linestatus
    auto l_linestatusKeep = std::make_unique<Aggregations::Keep>( context, l_linestatusIU );
    selection.push_back( l_linestatusKeep->getProduced() );

    // build a polynomial
    const unsigned rank = 6; // min 1
    const int seed = 9247;
    std::mt19937 gen(seed);
    std::uniform_real_distribution<> dis(0.1, 1.0);

    Sql::SqlType resultTy = Sql::getNumericFullLengthTy(4, true);

    // Horner's methode
    std::unique_ptr<Expression> polynomial;
    for (unsigned i = 0; i < rank; ++i) {
        double coeff = dis(gen);
        coeff = std::floor(coeff * 100.0 + 0.5) / 100.0;
        auto coeffValue = std::make_unique<Constant>(
            std::to_string(coeff), Sql::getNumericFullLengthTy(2, false) );
        auto x = std::make_unique<Identifier>(l_discountIU);

        if (polynomial) {
            polynomial = std::make_unique<Multiplication>(
                std::make_unique<Addition>(
                    std::move(polynomial),
                    std::move(coeffValue)
                ),
                std::move(x)
            );

            // cast
            polynomial = std::make_unique<Cast>( std::move(polynomial), resultTy );
        } else {
            polynomial = std::make_unique<Multiplication>(
                std::move(coeffValue),
                std::move(x)
            );
        }
    }

    auto aggr = std::make_unique<Aggregations::Sum>( context, std::move(polynomial) );
    selection.push_back( aggr->getProduced() );

    std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
    aggregations.push_back(std::move(l_returnflagKeep));
    aggregations.push_back(std::move(l_linestatusKeep));
    aggregations.push_back(std::move(aggr));

    auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

    auto result = std::make_unique<Result>( std::move(group), selection );
    verifyDependencies(*result);

    auto physicalTree = Algebra::translateToPhysicalTree(*result);
    physicalTree->produce();

    return funcGen.getFunction();
}

void benchmark_arithmetic_2_null(unsigned runs)
{
    auto db = load_tpch_null_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic2_func(*db);

    auto times = QueryCompiler::compileAndBenchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}

static llvm::Function * gen_arithmentic3_func(Database & db)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    QueryContext context(db);
    std::vector<iu_p_t> selection;

    Table * lineitem = db.getTable("lineitem");
    assert(lineitem != nullptr);
    auto scan = std::make_unique<TableScan>( context, *lineitem );
    addToScope(context, *scan);

    // collect ius
    iu_p_t l_returnflagIU = lookup(context, "l_returnflag");
    iu_p_t l_linestatusIU = lookup(context, "l_linestatus");
    iu_p_t l_linenumberIU = lookup(context, "l_linenumber");

    // keep l_returnflag
    auto l_returnflagKeep = std::make_unique<Aggregations::Keep>( context, l_returnflagIU );
    selection.push_back( l_returnflagKeep->getProduced() );

    // keep l_linestatus
    auto l_linestatusKeep = std::make_unique<Aggregations::Keep>( context, l_linestatusIU );
    selection.push_back( l_linestatusKeep->getProduced() );

    // build a polynomial
    const unsigned rank = 10; // min 1
    const int seed = 9247;
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> dis(1, 4);

    // Horner's methode
    std::unique_ptr<Expression> polynomial;
    for (unsigned i = 0; i < rank; ++i) {
        int coeff = dis(gen);
        auto coeffValue = std::make_unique<Constant>( std::to_string(coeff), Sql::getIntegerTy() );
        auto x = std::make_unique<Identifier>(l_linenumberIU);

        if (polynomial) {
            polynomial = std::make_unique<Multiplication>(
                std::make_unique<Addition>(
                    std::move(polynomial),
                    std::move(coeffValue)
                ),
                std::move(x)
            );
        } else {
            polynomial = std::make_unique<Multiplication>(
                std::move(coeffValue),
                std::move(x)
            );
        }
    }

    auto aggr = std::make_unique<Aggregations::Min>( context, std::move(polynomial) );
    selection.push_back( aggr->getProduced() );

    std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
    aggregations.push_back(std::move(l_returnflagKeep));
    aggregations.push_back(std::move(l_linestatusKeep));
    aggregations.push_back(std::move(aggr));

    auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

    auto result = std::make_unique<Result>( std::move(group), selection );
    verifyDependencies(*result);

    auto physicalTree = Algebra::translateToPhysicalTree(*result);
    physicalTree->produce();

    return funcGen.getFunction();
}

void benchmark_arithmetic_3_null(unsigned runs)
{
    auto db = load_tpch_null_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic3_func(*db);

    auto times = QueryCompiler::compileAndBenchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}

static llvm::Function * gen_arithmentic4_func(Database & db)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    QueryContext context(db);
    std::vector<iu_p_t> selection;

    Table * lineitem = db.getTable("lineitem");
    assert(lineitem != nullptr);
    auto scan = std::make_unique<TableScan>( context, *lineitem );
    addToScope(context, *scan);

    // collect ius
    iu_p_t l_returnflagIU = lookup(context, "l_returnflag");
    iu_p_t l_linestatusIU = lookup(context, "l_linestatus");
    iu_p_t l_linenumberIU = lookup(context, "l_linenumber");

    // keep l_returnflag
    auto l_returnflagKeep = std::make_unique<Aggregations::Keep>( context, l_returnflagIU );
    selection.push_back( l_returnflagKeep->getProduced() );

    // keep l_linestatus
    auto l_linestatusKeep = std::make_unique<Aggregations::Keep>( context, l_linestatusIU );
    selection.push_back( l_linestatusKeep->getProduced() );

    // build a polynomial
    const unsigned rank = 4; // min 1

    // Horner's methode
    std::unique_ptr<Expression> polynomial;
    for (unsigned i = 0; i < rank; ++i) {
        auto x = std::make_unique<Identifier>(l_linenumberIU);

        if (polynomial) {
            polynomial = std::make_unique<Multiplication>(
                std::move(polynomial),
                std::move(x)
            );
        } else {
            polynomial = std::move(x);
        }
    }

    auto aggr = std::make_unique<Aggregations::Min>( context, std::move(polynomial) );
    selection.push_back( aggr->getProduced() );

    std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
    aggregations.push_back(std::move(l_returnflagKeep));
    aggregations.push_back(std::move(l_linestatusKeep));
    aggregations.push_back(std::move(aggr));

    auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

    auto result = std::make_unique<Result>( std::move(group), selection );
    verifyDependencies(*result);

    auto physicalTree = Algebra::translateToPhysicalTree(*result);
    physicalTree->produce();

    return funcGen.getFunction();
}

void benchmark_arithmetic_4_null(unsigned runs)
{
    auto db = load_tpch_null_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = gen_arithmentic4_func(*db);

    auto times = QueryCompiler::compileAndBenchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}
