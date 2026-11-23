#include <unordered_map>

#include "codegen/LLVMCompat.hpp"
#include <iostream>

#include "codegen/CodeGen.hpp"
#include "foundations/Vector.hpp"
#include "third_party/hexdump.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "gtest/gtest.h"

using namespace Sql;

//-----------------------------------------------------------------------------
// test

static SqlType varchar_20_Ty = getVarcharTy(20);
static SqlType integerTy = getIntegerTy();

static value_op_t test_varchar1 = { };
static value_op_t test_varchar2 = { };
static value_op_t test_varchar3 = { };
static value_op_t test_int1 = { };
static value_op_t test_int2 = { };

static std::unique_ptr<Vector> test_vec1 = { };
static std::unique_ptr<Vector> test_vec2 = { };

static std::unordered_map<std::string, Vector *> tables;

static void init()
{
    test_varchar1 = Value::castString("Hello World", varchar_20_Ty);
    test_varchar2 = Value::castString("Hello 2", varchar_20_Ty);
    test_varchar3 = Value::castString("Hello World", varchar_20_Ty);
    test_int1 = Value::castString("42", getIntegerTy());
    test_int2 = Value::castString("43", getIntegerTy());

    test_vec1 = std::make_unique<Vector>(getValueSize(varchar_20_Ty), 2);
    test_vec2 = std::make_unique<Vector>(getValueSize(integerTy), 2);

    tables["column1"] = test_vec1.get();
    tables["column2"] = test_vec2.get();
}

static void fillColumns()
{
    llvm::Value * ptr1 = createPointerValue(test_vec1->at(0), toLLVMTy(varchar_20_Ty));
    test_varchar1->store(ptr1);

    llvm::Value * ptr2 = createPointerValue(test_vec1->at(1), toLLVMTy(varchar_20_Ty));
    test_varchar2->store(ptr2);

    llvm::Value * ptr3 = createPointerValue(test_vec2->at(0), toLLVMTy(integerTy));
    test_int1->store(ptr3);

    llvm::Value * ptr4 = createPointerValue(test_vec2->at(1), toLLVMTy(integerTy));
    test_int2->store(ptr4);
}

//-----------------------------------------------------------------------------
// store tuple test

static void * mem = new char[512]();

void printStoreResult()
{
    printf("\nmem:\n");
    hexdump(mem, 40);
}

static void storeTupleTest()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    llvm::Value * varcharPtr = createPointerValue( test_vec1->front(), toLLVMTy(varchar_20_Ty) );
    auto varcharVal = Value::load(varcharPtr, varchar_20_Ty);

    llvm::Value * intPtr = createPointerValue( test_vec2->front(), toLLVMTy(integerTy) );
    auto intVal = Value::load(intPtr, integerTy);

    /*Functions::genPrintfCall("Loaded values:\n");
    Utils::genPrintValue(*varcharVal);
    Functions::genPrintfCall("\n");
    Utils::genPrintValue(*intVal);
    Functions::genPrintfCall("\n");*/

    std::vector<std::unique_ptr<Value>> vals;
    vals.push_back( std::move(varcharVal) );
    vals.push_back( std::move(intVal) );
    SqlTuple tuple( std::move(vals));

    //Functions::genPrintfCall("Tuple size: %lu byte\n", cg_size_t(tuple.getSize()));

    llvm::Type * tupleTy = tuple.getType(); // store in HashJoin class
    llvm::Value * memPtr = createPointerValue(mem, tupleTy);

    tuple.store(memPtr);

    auto read = SqlTuple::load(memPtr, {varchar_20_Ty, integerTy});
    //Functions::genPrintfCall("Read tuple:\n");
    //genPrintSqlTuple(*read);

    // print hexdump
    //llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    //codeGen.CreateCall(&printStoreResult, funcTy);

    cg_hash_t hash = tuple.hash();
    cg_hash_t expected(11768261459832350955ul);

    funcGen.setReturnValue(codeGen->CreateICmpEQ(hash, expected));
}

llvm::Function * genPrintfRawTestFunc()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "typesPrintfRawTest", funcTy);

    init();

    fillColumns();

    llvm::Value * strPtr = test_varchar1->getLLVMValue();
    Functions::genPrintfCall("%.*s\n", cg_size_t(11), strPtr);

    funcGen.setReturnValue(cg_bool_t(true));

    return funcGen.getFunction();
}

llvm::Function * genHashTestFunc()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "typesHashTest", funcTy);

    init();

    fillColumns();

    static const char * str = "Hello World";

    value_op_t varcharVal = Value::castString(cg_ptr8_t::fromRawPointer(str), cg_size_t(strlen(str)), varchar_20_Ty);
    cg_hash_t hash = varcharVal->hash();
    cg_hash_t expected(test_varchar1->hash());

    funcGen.setReturnValue(codeGen->CreateICmpEQ(hash, expected));

    return funcGen.getFunction();
}

llvm::Function * genTupleTestFunc()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "typesTupleTest", funcTy);

    init();

    fillColumns();

    storeTupleTest();

    return funcGen.getFunction();
}

llvm::Function * genEqualsTestFunc()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "typesEqualsTest", funcTy);

    init();

    fillColumns();

    cg_bool_t eq = test_varchar1->equals(*test_varchar3);
    cg_bool_t ne = test_varchar2->equals(*test_varchar3);

    cg_bool_t result = eq && !ne;

    Functions::genPrintfCall("Equal test succeeded: %d",result);
    // TODO test all types

    funcGen.setReturnValue(result);

    return funcGen.getFunction();
}

TEST(TypesTest, PrintfRawTest) {
    ModuleGen moduleGen("PrintfRawTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * typesTest = genPrintfRawTestFunc();
    llvm::GenericValue resultTypesTest = QueryExecutor::executeFunction(typesTest,args);
    ASSERT_EQ(resultTypesTest.IntVal.getZExtValue(),1);
}

TEST(TypesTest, HashTest) {
    ModuleGen moduleGen("HashTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * typesTest = genHashTestFunc();
    llvm::GenericValue resultTypesTest = QueryExecutor::executeFunction(typesTest,args);
    ASSERT_EQ(resultTypesTest.IntVal.getZExtValue(),1);
}

TEST(TypesTest, TupleTest) {
    ModuleGen moduleGen("TupleTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * typesTest = genTupleTestFunc();
    llvm::GenericValue resultTypesTest = QueryExecutor::executeFunction(typesTest,args);
    ASSERT_EQ(resultTypesTest.IntVal.getZExtValue(),1);
}

TEST(TypesTest, EqualsTest) {
    ModuleGen moduleGen("EqualsTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * typesTest = genEqualsTestFunc();
    llvm::GenericValue resultTypesTest = QueryExecutor::executeFunction(typesTest,args);
    ASSERT_EQ(resultTypesTest.IntVal.getZExtValue(),1);
}
