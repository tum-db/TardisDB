#include <tuple>

#include <llvm/IR/TypeBuilder.h>
#include <sql/SqlType.hpp>

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"
#include "codegen/CodeGen.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlUtils.hpp"
#include "query_compiler/compiler.hpp"

#include "gtest/gtest.h"

using namespace Algebra::Physical;
using namespace Algebra::Physical::Expressions;

using namespace Sql;

static llvm::Function *genIntegerTest1Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerTest1", funcTy);

    iu_value_mapping_t emptyMapping;

    value_op_t val1(new Integer(6));
    value_op_t val2(new Integer(7));
    exp_op_t exp = std::make_unique<Addition>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->getLLVMValue());

    return funcGen.getFunction();
}

static llvm::Function *genIntegerTest2Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerTest2", funcTy);

    iu_value_mapping_t emptyMapping;

    value_op_t val1 = value_op_t(new Integer(6));
    value_op_t val2 = value_op_t(new Integer(7));
    exp_op_t exp = std::make_unique<Subtraction>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->getLLVMValue());

    return funcGen.getFunction();
}

static llvm::Function *genIntegerTest3Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerTest3", funcTy);

    iu_value_mapping_t emptyMapping;

    value_op_t val1 = value_op_t(new Integer(6));
    value_op_t val2 = value_op_t(new Integer(7));
    exp_op_t exp = std::make_unique<Multiplication>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->getLLVMValue());

    return funcGen.getFunction();
}

static llvm::Function *genIntegerTest4Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerTest4", funcTy);

    iu_value_mapping_t emptyMapping;

    value_op_t val1 = value_op_t(new Integer(5));
    value_op_t val2 = value_op_t(new Integer(2));
    exp_op_t exp = std::make_unique<Division>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->getLLVMValue());

    return funcGen.getFunction();
}

static llvm::Function *genIntegerTest5Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerTest5", funcTy);

    iu_value_mapping_t emptyMapping;

    value_op_t val1 = value_op_t(new Integer(5));
    value_op_t val2 = value_op_t(new Integer(2));
    exp_op_t exp = std::make_unique<Comparison>(
            Sql::getBoolTy(false),
            ComparisonMode::gtr,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->getLLVMValue());

    return funcGen.getFunction();
}

static llvm::Function *genNumericTest1Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "numericTest1", funcTy);

    iu_value_mapping_t emptyMapping;

    Sql::SqlType numericTy = Sql::getNumericTy(2, 1);

    Sql::SqlType resultTy;
    Sql::Operators::inferAddTy(numericTy, numericTy, resultTy);
    value_op_t val1 = Numeric::castString("6", numericTy);
    value_op_t val2 = Numeric::castString("7", numericTy);
    exp_op_t exp = std::make_unique<Addition>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->equals(*Numeric::castString("13.0", Sql::getNumericTy(2, 1))));

    return funcGen.getFunction();
}

static llvm::Function *genNumericTest2Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "numericTest2", funcTy);

    iu_value_mapping_t emptyMapping;

    Sql::SqlType numericTy = Sql::getNumericTy(2, 1);

    Sql::SqlType resultTy;
    Sql::Operators::inferSubTy(numericTy, numericTy, resultTy);
    value_op_t val1 = Numeric::castString("6", numericTy);
    value_op_t val2 = Numeric::castString("7", numericTy);
    exp_op_t exp = std::make_unique<Subtraction>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->equals(*Numeric::castString("-1.0", Sql::getNumericTy(2, 1))));

    return funcGen.getFunction();
}

static llvm::Function *genNumericTest3Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "numericTest3", funcTy);

    iu_value_mapping_t emptyMapping;

    Sql::SqlType numericTy = Sql::getNumericTy(2, 1);

    Sql::SqlType resultTy;
    Sql::Operators::inferMulTy(numericTy, numericTy, resultTy);
    value_op_t val1 = Numeric::castString("6.0", numericTy);
    value_op_t val2 = Numeric::castString("7.0", numericTy);
    exp_op_t exp = std::make_unique<Multiplication>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->equals(*Numeric::castString("42.0", Sql::getNumericTy(4, 2))));

    return funcGen.getFunction();
}

static llvm::Function *genNumericTest4Func() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "numericTest4", funcTy);

    iu_value_mapping_t emptyMapping;

    Sql::SqlType numericTy = Sql::getNumericTy(2, 1);

    Sql::SqlType resultTy;
    Sql::Operators::inferDivTy(numericTy, numericTy, resultTy);
    value_op_t val1 = Numeric::castString("5", numericTy);
    value_op_t val2 = Numeric::castString("2", numericTy);
    exp_op_t exp = std::make_unique<Division>(
            resultTy,
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->equals(*Numeric::castString("2.5", Sql::getNumericTy(2, 1))));

    return funcGen.getFunction();
}

static llvm::Function *genIntegerNullableTestFunc() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerNullableTest", funcTy);

    iu_value_mapping_t emptyMapping;

    value_op_t val1 = value_op_t(new Integer(6));
    value_op_t val2 = value_op_t(new Integer(7));
    val2 = Sql::Utils::createNullValue(std::move(val2));
    exp_op_t exp = std::make_unique<Subtraction>(
            Sql::getIntegerTy(true),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    funcGen.setReturnValue(result->equals(*Integer::castString("-1")));

    return funcGen.getFunction();
}

static llvm::Function *genIntegerOverflowTestFunc() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &llvmContext = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<void(), false>::get(llvmContext);
    FunctionGen funcGen(moduleGen, "integerOverflowTest", funcTy);

    iu_value_mapping_t emptyMapping;

    overflowFlag = false;
    value_op_t val1 = value_op_t(new Integer(2'005'000'000));
    value_op_t val2 = value_op_t(new Integer(2'005'000'000));
    exp_op_t exp = std::make_unique<Addition>(
            Sql::getIntegerTy(),
            std::make_unique<Constant>(std::move(val1)),
            std::make_unique<Constant>(std::move(val2))
    );
    value_op_t result = exp->evaluate(emptyMapping);

    cg_bool_t _result = genEvaluateOverflow();

    funcGen.setReturnValue(_result);

    overflowFlag = false;

    return funcGen.getFunction();
}

TEST(ExpressionTest, IntegerTest1) {
    ModuleGen moduleGen("IntegerTest1Module");
    llvm::Function *testFunc = genIntegerTest1Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getSExtValue(),13);
}

TEST(ExpressionTest, IntegerTest2) {
    ModuleGen moduleGen("IntegerTest2Module");
    llvm::Function *testFunc = genIntegerTest2Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    int64_t resultValue = result.IntVal.getSExtValue();
    ASSERT_EQ(resultValue,-1);
}

TEST(ExpressionTest, IntegerTest3) {
    ModuleGen moduleGen("IntegerTest3Module");
    llvm::Function *testFunc = genIntegerTest3Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getSExtValue(),42);
}

TEST(ExpressionTest, IntegerTest4) {
    ModuleGen moduleGen("IntegerTest4Module");
    llvm::Function *testFunc = genIntegerTest4Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    uint64_t intResult = result.IntVal.getSExtValue();
    ASSERT_EQ(result.IntVal.getSExtValue(), 2);
}

TEST(ExpressionTest, IntegerTest5) {
    ModuleGen moduleGen("IntegerTest5Module");
    llvm::Function *testFunc = genIntegerTest5Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getSExtValue(),-1);
}

TEST(ExpressionTest, NumericTest1) {
    ModuleGen moduleGen("NumericTest1Module");
    llvm::Function *testFunc = genNumericTest1Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

TEST(ExpressionTest, NumericTest2) {
    ModuleGen moduleGen("NumericTest2Module");
    llvm::Function *testFunc = genNumericTest2Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

TEST(ExpressionTest, NumericTest3) {
    ModuleGen moduleGen("NumericTest3Module");
    llvm::Function *testFunc = genNumericTest3Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

TEST(ExpressionTest, NumericTest4) {
    ModuleGen moduleGen("NumericTest4Module");
    llvm::Function *testFunc = genNumericTest4Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

TEST(ExpressionTest, NullableTest) {
    ModuleGen moduleGen("NullableTestModule");
    llvm::Function *testFunc = genIntegerNullableTestFunc();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

TEST(ExpressionTest, OverflowTest) {
    ModuleGen moduleGen("OverflowTestModule");
    llvm::Function *testFunc = genIntegerOverflowTestFunc();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

void expression_integer1_test() {
    ModuleGen moduleGen("IntegerTest1Module");
    llvm::Function *testFunc = genIntegerTest1Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6+7: " << result.IntVal.getSExtValue() << "\n";
}

void expression_integer2_test() {
    ModuleGen moduleGen("IntegerTest2Module");
    llvm::Function *testFunc = genIntegerTest2Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6-7: " << result.IntVal.getSExtValue() << "\n";
}

void expression_integer3_test() {
    ModuleGen moduleGen("IntegerTest3Module");
    llvm::Function *testFunc = genIntegerTest3Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6*7: " << result.IntVal.getSExtValue() << "\n";
}

void expression_integer4_test() {
    ModuleGen moduleGen("IntegerTest4Module");
    llvm::Function *testFunc = genIntegerTest4Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "5/2: " << result.IntVal.getSExtValue() << "\n";
}

void expression_integer5_test() {
    ModuleGen moduleGen("IntegerTest5Module");
    llvm::Function *testFunc = genIntegerTest5Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "5>2: " << result.IntVal.getSExtValue() << "\n";
}

void expression_numeric1_test() {
    ModuleGen moduleGen("NumericTest1Module");
    llvm::Function *testFunc = genNumericTest1Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6.0+7.0: " << result.IntVal.getZExtValue() << "\n";
}

void expression_numeric2_test() {
    ModuleGen moduleGen("NumericTest2Module");
    llvm::Function *testFunc = genNumericTest2Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6.0-7.0: " << result.IntVal.getSExtValue() << "\n";
}

void expression_numeric3_test() {
    ModuleGen moduleGen("NumericTest3Module");
    llvm::Function *testFunc = genNumericTest3Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6.0*7.0: " << result.IntVal.getSExtValue() << "\n";
}

void expression_numeric4_test() {
    ModuleGen moduleGen("NumericTest4Module");
    llvm::Function *testFunc = genNumericTest4Func();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "5.0/2.0: " << result.IntVal.getSExtValue() << "\n";
}

void expression_nullable_test() {
    ModuleGen moduleGen("NullableTestModule");
    llvm::Function *testFunc = genIntegerNullableTestFunc();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "6-7: " << result.IntVal.getSExtValue() << "\n";
}

void expression_overflow_test() {
    ModuleGen moduleGen("OverflowTestModule");
    llvm::Function *testFunc = genIntegerOverflowTestFunc();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryCompiler::compileAndExecuteReturn(testFunc, args);
    std::cout << "2'005'000'000+2'005'000'000: " << result.IntVal.getSExtValue() << "\n";
}

void expression_test() {
    expression_integer1_test();
    expression_integer2_test();
    expression_integer3_test();
    expression_integer4_test();
    expression_integer5_test();

    expression_numeric1_test();
    expression_numeric2_test();
    expression_numeric3_test();
    expression_numeric4_test();

    expression_nullable_test();
    expression_overflow_test();
}