#include <unordered_map>

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/Vector.hpp"
#include "foundations/InformationUnit.hpp"
#include "third_party/hexdump.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"
#include "queryExecutor/queryExecutor.hpp"

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

static void testScan(CodeGen & codeGen)
{
    auto & context = codeGen.getLLVMContext();
    auto & funcGen = codeGen.getCurrentFunctionGen();

//    llvm::Type * elemTy = varchar_20_td.getLLVMType(context); // store in IU
    llvm::Type * elemTy = toLLVMTy(varchar_20_Ty); // store in IU
    llvm::Type * columnTy = llvm::ArrayType::get(elemTy, test_vec1->size());

    llvm::Value * columnPtr = createPointerValue(test_vec1->front(), columnTy);

    Functions::genPrintfCall("columnPtr: %lu\n", columnPtr);

    cg_size_t tableSize(test_vec1->size());

#ifdef __APPLE__
    LoopGen loopGen(funcGen, {{"index", cg_size_t(0ull)}});
#else
    LoopGen loopGen(funcGen, {{"index", cg_size_t(0ul)}});
#endif
    cg_size_t index(loopGen.getLoopVar(0));
    {
        LoopBodyGen bodyGen(loopGen);

        Functions::genPrintfCall("scan iteration: %lu\n", index);

#ifdef __APPLE__
        llvm::Value * varcharPtr = codeGen->CreateGEP(columnTy, columnPtr, {cg_size_t(0ull), index});
#else
        llvm::Value * varcharPtr = codeGen->CreateGEP(columnTy, columnPtr, {cg_size_t(0ul), index});
#endif
        Functions::genPrintfCall("varchar ptr: %lu\n", varcharPtr);

        auto sqlval = Value::load(varcharPtr, varchar_20_Ty);
        Varchar * varcharVal = dynamic_cast<Varchar *>(sqlval.get());
        cg_size_t len(varcharVal->getLength()); // zero extend
        Functions::genPrintfCall("varchar length: %lu\n", len);
        Functions::genPrintfCall("varchar value: %.*s\n", len, sqlval->getLLVMValue());
        Functions::genPrintfCall("varchar hash: %lu\n", sqlval->hash());
    }
    cg_size_t nextIndex(index + 1ul);
    loopGen.loopDone(nextIndex < tableSize, {nextIndex});
}

// no longer relevant; this was just the implementation sketch for the TableScan operator
#if 0
static void doTestScan2(CodeGen & codeGen, const iu_set_t & produce)
{
    auto & context = codeGen.getLLVMContext();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    size_t tableSize = test_vec1->size(); // store in operator

    typedef std::tuple<SqlType, llvm::Type *, llvm::Value *> load_t;

    std::vector<load_t> to_load;
    for (auto iu : produce) {
        SqlType type = iu->sqlType;
        llvm::Type * elementTy = toLLVMTy(type);
        elementTy->dump();
        llvm::Type * columnTy = llvm::ArrayType::get(elementTy, tableSize);
        Vector * column = tables[iu->columnName];
        llvm::Value * columnPtr = createPointerValue(column, columnTy);
columnTy->dump();
        to_load.emplace_back(type, columnTy, columnPtr);
    }

    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
    cg_size_t index(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        Functions::genPrintfCall("scan iteration: %lu\n", index);

        std::vector<value_op_t> values;
        for (auto & t : to_load) {
            llvm::Value * elemPtr = codeGen->CreateGEP(std::get<1>(t), std::get<2>(t), index); // {cg_size_t(0ul), index});
            auto sqlValue = Value::load(elemPtr, std::get<0>(t));
            values.push_back(std::move(sqlValue));

            Functions::genPrintfCall("value: ");
            Utils::genPrintValue(*sqlValue);
            Functions::genPrintfCall("\n");
        }

        //parent->consume(values);
    }
    cg_size_t nextIndex = index + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}

static void testScan2(CodeGen & codeGen)
{
    auto iu1 = iuFactory.getIU("column1");
    auto iu2 = iuFactory.getIU("column2");
    iu_set_t p = {iu1, iu2};
    doTestScan2(codeGen, p);
}
#endif

//-----------------------------------------------------------------------------
// hash combine test

static void hashCombineTest(SqlTuple & tuple)
{
    auto & codeGen = getThreadLocalCodeGen();

    cg_hash_t hash = tuple.hash();
    cg_hash_t expected(11768261459832350955ul);

    Functions::genPrintfCall("hash: %lu - expected: %lu\n", hash, expected);
    Functions::genPrintfCall("passed: %d\n", codeGen->CreateICmpEQ(hash, expected));
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

    llvm::Value * varcharPtr = createPointerValue( test_vec1->front(), toLLVMTy(varchar_20_Ty) );
    auto varcharVal = Value::load(varcharPtr, varchar_20_Ty);

    llvm::Value * intPtr = createPointerValue( test_vec2->front(), toLLVMTy(integerTy) );
    auto intVal = Value::load(intPtr, integerTy);

    Functions::genPrintfCall("Loaded values:\n");
    Utils::genPrintValue(*varcharVal);
    Functions::genPrintfCall("\n");
    Utils::genPrintValue(*intVal);
    Functions::genPrintfCall("\n");

    std::vector<std::unique_ptr<Value>> vals;
    vals.push_back( std::move(varcharVal) );
    vals.push_back( std::move(intVal) );
    SqlTuple tuple( std::move(vals));

    Functions::genPrintfCall("Tuple size: %lu byte\n", cg_size_t(tuple.getSize()));

    llvm::Type * tupleTy = tuple.getType(); // store in HashJoin class
    llvm::Value * memPtr = createPointerValue(mem, tupleTy);

    tuple.store(memPtr);

    auto read = SqlTuple::load(memPtr, {varchar_20_Ty, integerTy});
    Functions::genPrintfCall("Read tuple:\n");
    genPrintSqlTuple(*read);

    hashCombineTest(*read);

    // print hexdump
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    codeGen.CreateCall(&printStoreResult, funcTy);
}

//-----------------------------------------------------------------------------
// equals test

static void testEquals()
{
    cg_bool_t eq = test_varchar1->equals(*test_varchar3);
    cg_bool_t ne = test_varchar2->equals(*test_varchar3);

    cg_bool_t result = eq && !ne;

    // TODO test all types

    Functions::genPrintfCall("passed: %d\n", result);
}

static void testLoad(ModuleGen & moduleGen)
{
    auto & codeGen = moduleGen.getCodeGen();
    auto & context = codeGen.getLLVMContext();

    static const char * str = "Hello World";

    value_op_t varcharVal = Value::castString(cg_ptr8_t::fromRawPointer(str), cg_size_t(strlen(str)), varchar_20_Ty);
    cg_hash_t hash = varcharVal->hash();
    cg_hash_t expected(test_varchar1->hash());

    Functions::genPrintfCall("hash: %lu - expected: %lu\n", hash, expected);
    Functions::genPrintfCall("passed: %d\n", codeGen->CreateICmpEQ(hash, expected));
}

static void genTest(ModuleGen & moduleGen)
{
    auto & codeGen = moduleGen.getCodeGen();
    auto & context = codeGen.getLLVMContext();

    Functions::genPrintfCall("=== printf raw test ===\n");
    llvm::Value * strPtr = test_varchar1->getLLVMValue();
    Functions::genPrintfCall("%.*s\n", cg_size_t(11), strPtr);

    Functions::genPrintfCall("=== load/hash value test ===\n");
    testLoad(moduleGen);

    Functions::genPrintfCall("=== scan ===\n");
    testScan(codeGen);

    Functions::genPrintfCall("=== store/load tuple test ===\n");
    storeTupleTest();

    Functions::genPrintfCall("=== test equals() ===\n");
    testEquals();
}

llvm::Function * genTypesTestFunc(Database & db)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "typesTest", funcTy);

    init();
    fillColumns();

    genTest(moduleGen);

    return funcGen.getFunction();
}

void types_test()
{
    std::unique_ptr<Database> db;

    ModuleGen moduleGen("TypesTestModule");
    llvm::Function * queryFunc = genTypesTestFunc(*db);
    std::vector<llvm::GenericValue> args;
    QueryExecutor::executeFunction(queryFunc,args);
}
