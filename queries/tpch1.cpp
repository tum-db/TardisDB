
#include <llvm/IR/TypeBuilder.h>

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/loader.hpp"
#include "query_compiler/compiler.hpp"
#include "queries/common.hpp"

using namespace Algebra::Logical;
using namespace Algebra::Logical::Aggregations;
using namespace Algebra::Logical::Expressions;

/*
-- TPC-H Query 1

select
        l_returnflag,
        l_linestatus,
        sum(l_quantity) as sum_qty,
        sum(l_extendedprice) as sum_base_price,
        sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
        sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
        avg(l_quantity) as avg_qty,
        avg(l_extendedprice) as avg_price,
        avg(l_discount) as avg_disc,
        count(*) as count_order
from
        lineitem
where
        l_shipdate <= date '1998-12-01' - interval '90' day
group by
        l_returnflag,
        l_linestatus
order by
        l_returnflag,
        l_linestatus
*/

static llvm::Function * genFunc(Database & db)
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
    iu_p_t l_quantityIU      = lookup(context, "l_quantity");
    iu_p_t l_extendedpriceIU = lookup(context, "l_extendedprice");
    iu_p_t l_discountIU      = lookup(context, "l_discount");
    iu_p_t l_taxIU           = lookup(context, "l_tax");
    iu_p_t l_shipdateIU      = lookup(context, "l_shipdate");

    // select operator
    // where l_shipdate <= date '1998-12-01' - interval '90' day
    auto dateExp = std::make_unique<Comparison>(
            ComparisonMode::leq,
            std::make_unique<Identifier>(l_shipdateIU),
            std::make_unique<Constant>("1998-09-02", Sql::getDateTy())
    );
    auto select = std::make_unique<Select>( std::move(scan), std::move(dateExp) );

    // keep l_returnflag
    auto l_returnflagKeep = std::make_unique<Aggregations::Keep>( context, l_returnflagIU );
    selection.push_back( l_returnflagKeep->getProduced() );

    // keep l_linestatus
    auto l_linestatusKeep = std::make_unique<Aggregations::Keep>( context, l_linestatusIU );
    selection.push_back( l_linestatusKeep->getProduced() );

    // sum(l_quantity) as sum_qty,
    auto sum_qtyExp = std::make_unique<Identifier>(l_quantityIU);
    auto sum_qty = std::make_unique<Aggregations::Sum>( context, std::move(sum_qtyExp) );
    selection.push_back( sum_qty->getProduced() );

    // sum(l_extendedprice) as sum_base_price,
    auto sum_base_priceExp = std::make_unique<Identifier>(l_extendedpriceIU);
    auto sum_base_price = std::make_unique<Aggregations::Sum>( context, std::move(sum_base_priceExp) );
    selection.push_back( sum_base_price->getProduced() );

    // sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
    auto sum_disc_priceExp = std::make_unique<Multiplication>(
            std::make_unique<Identifier>(l_extendedpriceIU),
            std::make_unique<Subtraction>(
                    std::make_unique<Constant>( "1", Sql::toNotNullableTy( getType(l_discountIU) ) ),
                    std::make_unique<Identifier>(l_discountIU)
            )
    );
    auto sum_disc_price = std::make_unique<Aggregations::Sum>( context, std::move(sum_disc_priceExp) );
    selection.push_back( sum_disc_price->getProduced() );

    // sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
    auto sum_chargeExp = std::make_unique<Multiplication>(
            std::make_unique<Identifier>(l_extendedpriceIU),
            std::make_unique<Multiplication>(
                    std::make_unique<Subtraction>(
                            std::make_unique<Constant>( "1", Sql::toNotNullableTy( getType(l_discountIU) ) ),
                            std::make_unique<Identifier>(l_discountIU)
                    ),
                    std::make_unique<Addition>(
                            std::make_unique<Constant>( "1", Sql::toNotNullableTy( getType(l_taxIU) ) ),
                            std::make_unique<Identifier>(l_taxIU)
                    )
            )
    );
    auto sum_charge = std::make_unique<Aggregations::Sum>( context, std::move(sum_chargeExp) );
    selection.push_back( sum_charge->getProduced() );

    // avg(l_quantity) as avg_qty,
    auto avg_qtyExp = std::make_unique<Identifier>(l_quantityIU);
    auto avg_qty = std::make_unique<Aggregations::Avg>( context, std::move(avg_qtyExp) );
    selection.push_back( avg_qty->getProduced() );

    // avg(l_extendedprice) as avg_price,
    auto avg_priceExp = std::make_unique<Identifier>(l_extendedpriceIU);
    auto avg_price = std::make_unique<Aggregations::Avg>( context, std::move(avg_priceExp) );
    selection.push_back( avg_price->getProduced() );

    // avg(l_discount) as avg_disc,
    auto avg_discExp = std::make_unique<Identifier>(l_discountIU);
    auto avg_disc = std::make_unique<Aggregations::Avg>( context, std::move(avg_discExp) );
    selection.push_back( avg_disc->getProduced() );

    // count(*) as count_order
    auto count_order = std::make_unique<Aggregations::CountAll>(context);
    selection.push_back( count_order->getProduced() );

    std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
    aggregations.push_back(std::move(l_returnflagKeep));
    aggregations.push_back(std::move(l_linestatusKeep));
    aggregations.push_back(std::move(sum_qty));
    aggregations.push_back(std::move(sum_base_price));
    aggregations.push_back(std::move(sum_disc_price));
    aggregations.push_back(std::move(sum_charge));
    aggregations.push_back(std::move(avg_qty));
    aggregations.push_back(std::move(avg_price));
    aggregations.push_back(std::move(avg_disc));
    aggregations.push_back(std::move(count_order));

    auto group = std::make_unique<GroupBy>( std::move(select), std::move(aggregations) );

    auto result = std::make_unique<Result>( std::move(group), selection );
    verifyDependencies(*result);

    auto physicalTree = Algebra::translateToPhysicalTree(*result);
    physicalTree->produce();

    return funcGen.getFunction();
}

void benchmark_tpc_h_1(unsigned runs)
{
    auto db = load_tpch_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = genFunc(*db);

    auto times = QueryCompiler::compileAndBenchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}

void run_tpc_h_1()
{
    auto db = load_tpch_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = genFunc(*db);
    QueryCompiler::compileAndExecute(queryFunc);
}

void benchmark_tpc_h_1_null(unsigned runs)
{
    auto db = load_tpch_null_db();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = genFunc(*db);

    auto times = QueryCompiler::compileAndBenchmark(queryFunc, runs);
    printf("average run time: %f ms\n", times.executionTime / 1000.0);
}

void run_tpc_h_1_null()
{
    auto db = load_tpch_null_db();

    printf("running query...\n");
    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = genFunc(*db);
    QueryCompiler::compileAndExecute(queryFunc);
}
