
#include <llvm/IR/TypeBuilder.h>

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "queries/common.hpp"
#include "semanticAnalyser/SemanticalVerifier.hpp"

using namespace Algebra::Logical;
using namespace Algebra::Logical::Aggregations;
using namespace Algebra::Logical::Expressions;

/*
select user_name
from users u, versiontable v
where u.rid = v.rid
    and v.tableid = 'users'
    and v.vid = 'v2'
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

    Table * versiontable = db.getTable("Versiontable");
    assert(versiontable != nullptr);
    auto versiontableScan = std::make_unique<TableScan>(context, *versiontable);
    semanticalAnalysis::addToScope(context, *versiontableScan, "u");

    Table * usertable = db.getTable("users");
    assert(usertable != nullptr);
    auto usertableScan = std::make_unique<TableScan>(context, *usertable);
    semanticalAnalysis::addToScope(context, *usertableScan, "v");

    // collect ius
    iu_p_t u_rid     = semanticalAnalysis::lookup(context, "u.rid");
    iu_p_t v_rid     = semanticalAnalysis::lookup(context, "v.rid");
    iu_p_t v_tableid = semanticalAnalysis::lookup(context, "v.tableid");
    iu_p_t v_vid     = semanticalAnalysis::lookup(context, "v.vid");

    auto vidExpr = std::make_unique<Comparison>(
        ComparisonMode::eq,
        std::make_unique<Identifier>(v_vid),
        std::make_unique<Constant>("2", Sql::getIntegerTy())
    );

    auto tableidExpr = std::make_unique<Comparison>(
        ComparisonMode::eq,
        std::make_unique<Identifier>(v_tableid),
        std::make_unique<Constant>("users", Sql::getVarcharTy(32))
    );

    // TODO operator implementation
    auto versiontableWhereExpr = std::make_unique<And>(
        std::move(vidExpr),
        std::move(tableidExpr)
    );

    auto versiontableSelect = std::make_unique<Select>(
        std::move(versiontableScan),
        std::move(versiontableWhereExpr)
    );

    auto joinExpr = std::make_unique<Comparison>(
        ComparisonMode::eq,
        std::make_unique<Identifier>(v_rid), // left side
        std::make_unique<Identifier>(u_rid) // right side
    );
    join_expr_vec_t joinExprVec;
    joinExprVec.push_back(std::move(joinExpr));
    auto join = std::make_unique<Join>(
        std::move(versiontableSelect), // build side
        std::move(usertableScan), // probe side
        std::move(joinExprVec),
        Join::Method::Hash
    );

    auto result = std::make_unique<Result>(std::move(join), selection);
    verifyDependencies(*result);

    auto physicalTree = Algebra::translateToPhysicalTree(*result);
    physicalTree->produce();

    return funcGen.getFunction();
}

void benchmark_versioned_1(unsigned runs)
{
    throw NotImplementedException();
}

void run_versioned_1()
{
    auto db = load_tables();

    ModuleGen moduleGen("QueryModule");
    llvm::Function * queryFunc = genFunc(*db);
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryExecutor::executeFunction(queryFunc,args);
}
