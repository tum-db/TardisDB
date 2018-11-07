#include <tuple>

#include <llvm/IR/TypeBuilder.h>

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"
#include "codegen/CodeGen.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlUtils.hpp"
#include "query_compiler/compiler.hpp"

using namespace Algebra::Physical;
using namespace Algebra::Physical::Expressions;

using namespace Sql;

static llvm::Function * genFunc()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "expressionTest", funcTy);

    iu_value_mapping_t emptyMapping;

    // Integer
    Functions::genPrintfCall("Integer:\n");

    value_op_t val1( new Integer(6) );
    value_op_t val2( new Integer(7) );
    exp_op_t exp = std::make_unique<Addition>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6+7: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    val1 = value_op_t( new Integer(6) );
    val2 = value_op_t( new Integer(7) );
    exp = std::make_unique<Subtraction>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6-7: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    val1 = value_op_t( new Integer(6) );
    val2 = value_op_t( new Integer(7) );
    exp = std::make_unique<Multiplication>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6*7: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    val1 = value_op_t( new Integer(5) );
    val2 = value_op_t( new Integer(2) );
    exp = std::make_unique<Division>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("5/2: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    val1 = value_op_t( new Integer(5) );
    val2 = value_op_t( new Integer(2) );
    exp = std::make_unique<Comparison>(
            Sql::getBoolTy(false),
            ComparisonMode::gtr,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("5>2: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    // Numeric
    Functions::genPrintfCall("Numeric:\n");

    Sql::SqlType numericTy = Sql::getNumericTy(2, 1);

    Sql::SqlType resultTy;
    Sql::Operators::inferAddTy(numericTy, numericTy, resultTy);
    val1 = Numeric::castString("6", numericTy);
    val2 = Numeric::castString("7", numericTy);
    exp = std::make_unique<Addition>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6.0+7.0: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    Sql::Operators::inferSubTy(numericTy, numericTy, resultTy);
    val1 = Numeric::castString("6", numericTy);
    val2 = Numeric::castString("7", numericTy);
    exp = std::make_unique<Subtraction>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6.0-7.0: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    Sql::Operators::inferMulTy(numericTy, numericTy, resultTy);
    val1 = Numeric::castString("6", numericTy);
    val2 = Numeric::castString("7", numericTy);
    exp = std::make_unique<Multiplication>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6.0*7.0: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    Sql::Operators::inferDivTy(numericTy, numericTy, resultTy);
    val1 = Numeric::castString("5", numericTy);
    val2 = Numeric::castString("2", numericTy);
    exp = std::make_unique<Division>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("5.0/2.0: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    // nullable integer
    Functions::genPrintfCall("Nullable Integer:\n");

    val1 = value_op_t( new Integer(6) );
    val2 = value_op_t( new Integer(7) );
    val2 = Sql::Utils::createNullValue( std::move(val2) );
    exp = std::make_unique<Subtraction>(
            Sql::getIntegerTy(true),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("6-7: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");

    // overflow
    Functions::genPrintfCall("Integer overflow:\n");

    overflowFlag = false;
    val1 = value_op_t( new Integer(2'005'000'000) );
    val2 = value_op_t( new Integer(2'005'000'000) );
    exp = std::make_unique<Addition>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    result = exp->evaluate(emptyMapping);
    Functions::genPrintfCall("2'005'000'000+2'005'000'000: ");
    Utils::genPrintValue(*result);
    Functions::genPrintfCall("\n");
    genOverflowEvaluation();
    overflowFlag = false;

    return funcGen.getFunction();
}

void expression_test()
{
    ModuleGen moduleGen("TestModule");
    llvm::Function * testFunc = genFunc();
    QueryCompiler::compileAndExecute(testFunc);
}
