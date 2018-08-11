
#include <llvm/IR/TypeBuilder.h>

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/loader.hpp"
#include "query_compiler/compiler.hpp"

using namespace Algebra::Logical;
using namespace Algebra::Logical::Aggregations;
using namespace Algebra::Logical::Expressions;

llvm::Function * genFunc(Database & db)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    Functions::genPrintfCall("select count(*) from studenten\n");
    {
        QueryContext context(db);
        std::vector<iu_p_t> selection;

        Table * studenten = db.getTable("studenten");
        assert(studenten != nullptr);
        auto scan = std::make_unique<TableScan>( context, *studenten );
        addToScope(context, *scan);

        auto count = std::make_unique<Aggregations::CountAll>(context);
        selection.push_back( count->getProduced() );

        std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
        aggregations.push_back(std::move(count));
        auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

        auto result = std::make_unique<Result>( std::move(group), selection );
        verifyDependencies(*result);

        auto physicalTree = Algebra::translateToPhysicalTree(*result);
        physicalTree->produce();
    }

    Functions::genPrintfCall("select sum(semester) from studenten\n");
    {
        QueryContext context(db);
        std::vector<iu_p_t> selection;

        Table * studenten = db.getTable("studenten");
        assert(studenten != nullptr);
        auto scan = std::make_unique<TableScan>( context, *studenten );
        addToScope(context, *scan);

        iu_p_t semesterIU = lookup(context, "semester");
        auto sumExp = std::make_unique<Identifier>(semesterIU);
        auto sum = std::make_unique<Aggregations::Sum>( context, std::move(sumExp) );
        selection.push_back( sum->getProduced() );

        std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
        aggregations.push_back(std::move(sum));
        auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

        auto result = std::make_unique<Result>( std::move(group), selection );
        verifyDependencies(*result);

        auto physicalTree = Algebra::translateToPhysicalTree(*result);
        physicalTree->produce();
    }

    Functions::genPrintfCall("select avg(semester) from studenten\n");
    {
        QueryContext context(db);
        std::vector<iu_p_t> selection;

        Table * studenten = db.getTable("studenten");
        assert(studenten != nullptr);
        auto scan = std::make_unique<TableScan>( context, *studenten );
        addToScope(context, *scan);
        iu_p_t semesterIU = lookup(context, "semester");

        auto avgExp = std::make_unique<Identifier>(semesterIU);
        auto avg = std::make_unique<Aggregations::Avg>( context, std::move(avgExp) );
        selection.push_back( avg->getProduced() );

        std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
        aggregations.push_back(std::move(avg));
        auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

        auto result = std::make_unique<Result>( std::move(group), selection );
        verifyDependencies(*result);

        auto physicalTree = Algebra::translateToPhysicalTree(*result);
        physicalTree->produce();
    }

    Functions::genPrintfCall("select count(*), sum(semester), avg(semester) from studenten\n");
    {
        QueryContext context(db);
        std::vector<iu_p_t> selection;

        Table * studenten = db.getTable("studenten");
        assert(studenten != nullptr);
        auto scan = std::make_unique<TableScan>( context, *studenten );
        addToScope(context, *scan);
        iu_p_t semesterIU = lookup(context, "semester");

        auto count = std::make_unique<Aggregations::CountAll>(context);
        selection.push_back( count->getProduced() );

        auto sumExp = std::make_unique<Identifier>(semesterIU);
        auto sum = std::make_unique<Aggregations::Sum>( context, std::move(sumExp) );
        selection.push_back( sum->getProduced() );

        auto avgExp = std::make_unique<Identifier>(semesterIU);
        auto avg = std::make_unique<Aggregations::Avg>( context, std::move(avgExp) );
        selection.push_back( avg->getProduced() );

        std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
        aggregations.push_back(std::move(count));
        aggregations.push_back(std::move(sum));
        aggregations.push_back(std::move(avg));
        auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

        auto result = std::make_unique<Result>( std::move(group), selection );
        verifyDependencies(*result);

        auto physicalTree = Algebra::translateToPhysicalTree(*result);
        physicalTree->produce();
    }

    Functions::genPrintfCall("select semester, count(*) from studenten group by semester\n");
    {
        QueryContext context(db);
        std::vector<iu_p_t> selection;

        Table * studenten = db.getTable("studenten");
        assert(studenten != nullptr);
        auto scan = std::make_unique<TableScan>( context, *studenten );
        addToScope(context, *scan);
        iu_p_t semesterIU = lookup(context, "semester");

        auto keep = std::make_unique<Aggregations::Keep>( context, semesterIU );
        selection.push_back( keep->getProduced() );

        auto count = std::make_unique<Aggregations::CountAll>(context);
        selection.push_back( count->getProduced() );

        std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations;
        aggregations.push_back(std::move(keep));
        aggregations.push_back(std::move(count));
        auto group = std::make_unique<GroupBy>( std::move(scan), std::move(aggregations) );

        auto result = std::make_unique<Result>( std::move(group), selection );
        verifyDependencies(*result);

        auto physicalTree = Algebra::translateToPhysicalTree(*result);
        physicalTree->produce();
    }

    return funcGen.getFunction();
}

void groupby_test()
{
    std::unique_ptr<Database> db;

    try {
        printf("loading data into tables...\n");
        db = loadUniDb();
        printf("done\n");
    } catch (const std::exception & e) {
        fprintf(stderr, "%s\n", e.what());
        throw;
    }

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = genFunc(*db);
    QueryCompiler::compileAndExecute(queryFunc);
}
