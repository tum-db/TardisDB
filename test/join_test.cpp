
#include <llvm/IR/TypeBuilder.h>
#include <iostream>

#include "codegen/CodeGen.hpp"
#include "foundations/StaticHashtable.hpp"
#include "foundations/MemoryPool.hpp"
#include "foundations/Vector.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"
#include "sql/SqlUtils.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "gtest/gtest.h"
// This test also represents the draft for the HashJoin operator

using namespace Sql;


// table 1:

void initTables(std::vector<value_op_t> &persistentStore, Vector &table1_column1, Vector &table1_column2, Vector &table1_column3,
                Vector &table2_column1, Vector &table2_column2, Vector &table2_column3) {
    persistentStore.push_back(Text::castString("left1", getTextTy()));
    persistentStore.push_back(Text::castString("left2", getTextTy()));
    persistentStore.push_back(Text::castString("left3", getTextTy()));
    persistentStore.push_back(Text::castString("left4", getTextTy()));
    persistentStore.push_back(Text::castString("left5", getTextTy()));
    persistentStore.push_back(Text::castString("left6", getTextTy()));
    persistentStore.push_back(Text::castString("left7", getTextTy()));
    persistentStore.push_back(Text::castString("left8", getTextTy()));
    persistentStore.push_back(Text::castString("left9", getTextTy()));

    persistentStore[0]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(0)));
    persistentStore[1]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(1)));
    persistentStore[2]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(2)));
    persistentStore[3]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(3)));
    persistentStore[4]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(4)));
    persistentStore[5]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(5)));
    persistentStore[6]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(6)));
    persistentStore[7]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(7)));
    persistentStore[8]->store(cg_voidptr_t::fromRawPointer(table1_column1.at(8)));

    (new Integer(1))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(0)));
    (new Integer(2))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(1)));
    (new Integer(2))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(2)));
    (new Integer(3))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(3)));
    (new Integer(4))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(4)));
    (new Integer(5))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(5)));
    (new Integer(12))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(6)));
    (new Integer(13))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(7)));
    (new Integer(12))->store(cg_voidptr_t::fromRawPointer(table1_column2.at(8)));

    (new Integer(6))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(0)));
    (new Integer(7))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(1)));
    (new Integer(7))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(2)));
    (new Integer(8))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(3)));
    (new Integer(9))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(4)));
    (new Integer(10))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(7)));
    (new Integer(12))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(6)));
    (new Integer(13))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(7)));
    (new Integer(12))->store(cg_voidptr_t::fromRawPointer(table1_column3.at(8)));


    persistentStore.push_back(Text::castString("right1", getTextTy()));
    persistentStore.push_back(Text::castString("right2", getTextTy()));
    persistentStore.push_back(Text::castString("right3", getTextTy()));
    persistentStore.push_back(Text::castString("right4", getTextTy()));
    persistentStore.push_back(Text::castString("right5", getTextTy()));
    persistentStore.push_back(Text::castString("right6", getTextTy()));

    persistentStore[9]->store(cg_voidptr_t::fromRawPointer(table2_column1.at(0)));
    persistentStore[10]->store(cg_voidptr_t::fromRawPointer(table2_column1.at(1)));
    persistentStore[11]->store(cg_voidptr_t::fromRawPointer(table2_column1.at(2)));
    persistentStore[12]->store(cg_voidptr_t::fromRawPointer(table2_column1.at(3)));
    persistentStore[13]->store(cg_voidptr_t::fromRawPointer(table2_column1.at(4)));
    persistentStore[14]->store(cg_voidptr_t::fromRawPointer(table2_column1.at(5)));

    (new Integer(1))->store(cg_voidptr_t::fromRawPointer(table2_column2.at(0)));
    (new Integer(2))->store(cg_voidptr_t::fromRawPointer(table2_column2.at(1)));
    (new Integer(2))->store(cg_voidptr_t::fromRawPointer(table2_column2.at(2)));
    (new Integer(3))->store(cg_voidptr_t::fromRawPointer(table2_column2.at(3)));
    (new Integer(3))->store(cg_voidptr_t::fromRawPointer(table2_column2.at(4)));
    (new Integer(4))->store(cg_voidptr_t::fromRawPointer(table2_column2.at(5)));

    (new Integer(6))->store(cg_voidptr_t::fromRawPointer(table2_column3.at(0)));
    (new Integer(7))->store(cg_voidptr_t::fromRawPointer(table2_column3.at(1)));
    (new Integer(7))->store(cg_voidptr_t::fromRawPointer(table2_column3.at(2)));
    (new Integer(8))->store(cg_voidptr_t::fromRawPointer(table2_column3.at(3)));
    (new Integer(8))->store(cg_voidptr_t::fromRawPointer(table2_column3.at(4)));
    (new Integer(11))->store(cg_voidptr_t::fromRawPointer(table2_column3.at(5)));
}

// join: table1_column2 == table2_column2 and table1_column3 == table2_column3
/*
expected:
left1, right1
left2, right2
left2, right3
left3, right2
left3, right3
left4, right4
left4, right5

RESULT: left1, right1
RESULT: left2, right2
RESULT: left3, right2
RESULT: left2, right3
RESULT: left3, right3
RESULT: left4, right4
RESULT: left4, right5
*/

#if 0
void HashJoin::produce(ostream & stream)
{
   stream << "std::unordered_multimap< std::tuple<\n";

   bool first = true;
   for (const auto & p : joinPairs) {
      if (!first) {
         stream << ",\n";
      }
      first = false;
      stream << to_cpp_type(p.first);
      stream << " /* " << p.first->name << " */";
   }
   stream << ">, \n std::tuple<";

   first = true;
   for (auto iu : getStored()) {
      if (!first) {
         stream << ",\n";
      }
      first = false;
      stream << to_cpp_type(iu);
      stream << " /* " << iu->name << " */";
   }

   stream << ">> hashJoin" << opId << ";\n";

   leftChild->produce(stream);

   // build hashtable here

   rightChild->produce(stream);
}

extern "C" void query() {
  std::unordered_multimap<std::tuple<Integer /* c_id */>,
                          std::tuple<Varchar<16> /* c_first */>>
      hashJoin5;
  for (auto tid3 = 0ul, size3 = customer_table->size(); tid3 < size3; ++tid3) {
    auto c_id = customer_table->c_id_column[tid3];
    auto c_first = customer_table->c_first_column[tid3];
    // hash join left begin
    const auto join_attributes5 = std::make_tuple(c_id);
    auto tuple5 = std::make_tuple(c_first);
    hashJoin5.insert(std::make_pair(join_attributes5, tuple5));
    // hash join left end
  }

  // build hashtable here


  for (auto tid4 = 0ul, size4 = order_table->size(); tid4 < size4; ++tid4) {
    auto o_id = order_table->o_id_column[tid4];
    // hash join right begin
    const auto join_attributes5 = std::make_tuple(o_id);
    const auto range = hashJoin5.equal_range(join_attributes5);
    for (auto it = range.first; it != range.second; ++it) {
      const auto &tuple5 = it->second;
      auto c_first = std::get<0>(tuple5);
      std::cout << c_first << std::endl;
    }
    // hash join right end
  }
}
#endif

extern llvm::Value * genCreateListHeader(cg_voidptr_t memoryPool);
extern llvm::Type * getListNodeTy(llvm::Type * tupleTy);
extern void genAppendEntryToList(cg_voidptr_t memoryPool, llvm::Value * headerPtr, llvm::Type * nodeTy, cg_hash_t hash, SqlTuple & tuple);
extern std::pair<cg_voidptr_t, cg_size_t> genListGetHeaderData(llvm::Value * headerPtr);

static void printResult(cg_voidptr_t p)
{
    Functions::genPrintfCall("===\niter node: %p\n===\n", p);
}

static std::vector<Value *> rightProduced;

static llvm::Type * listNodeTy;

static void doProbe(cg_voidptr_t p, Vector &table1_column1, Vector &table1_column2, Vector &table1_column3)
{
    Functions::genPrintfCall("===\ndoProbe() iter node: %p\n===\n", p);

    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    std::vector<SqlType> tds = {
            getTextTy(),
            std::make_unique<Integer>(1)->type,
            std::make_unique<Integer>(1)->type
    };

//    llvm::Type * nodeTy = getListNodeTy( SqlTuple::getType(tds) );
    llvm::Type * nodePtrTy = llvm::PointerType::getUnqual(listNodeTy);

    llvm::Value * nodePtr = codeGen->CreatePointerCast(p, nodePtrTy);
    llvm::Value * tuplePtr = codeGen->CreateStructGEP(listNodeTy, nodePtr, 2);

    auto t = SqlTuple::load(tuplePtr, tds);

    // real match?
    cg_bool_t all(true);
    // use join attributes vector

//    all = all && t.values[1].equals( *rightProduced[1] );
//    all = all && t.values[2].equals( *rightProduced[2] );

    for (size_t i = 1; i < 3; ++i) {
        all = all && t->values[i]->equals( *rightProduced[i] );
    }

    funcGen.setReturnValue(all);

    IfGen check(funcGen, all);
    {
        Functions::genPrintfCall("RESULT: ");
        Utils::genPrintValue(*(t->values[0]));
        Functions::genPrintfCall(", ");
        Utils::genPrintValue(*rightProduced[0]);
        Functions::genPrintfCall("\n");


        // parent.consume(SqlValue's)
    }
    check.EndIf();
}

llvm::Function * genJoinTest(std::vector<value_op_t> &persistentStore, Vector &table1_column1, Vector &table1_column2, Vector &table1_column3,
                             Vector &table2_column1, Vector &table2_column2, Vector &table2_column3)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "joinTest", funcTy);

    initTables(persistentStore, table1_column1, table1_column2, table1_column3, table2_column1, table2_column2,table2_column3);
    //std::vector join_attr_indices { 1, 2 };

    //[ join produce start
    cg_voidptr_t memoryPool = genMemoryPoolCreateCall();
    llvm::Value * listHeaderPtr = genCreateListHeader(memoryPool);

    //[ scan left (produce) start
    //[ loop start
    auto elem1Td = Text::castString("right1", getTextTy()); // store
    llvm::Type * column1Ty = llvm::ArrayType::get(elem1Td->getLLVMType(), table1_column1.size()); // store
    llvm::Value * column1Ptr = createPointerValue(table1_column1.front(), column1Ty); // store

    auto elem2Td = std::make_unique<Integer>(1);
    llvm::Type * column2Ty = llvm::ArrayType::get(elem2Td->getLLVMType(), table1_column2.size());
    llvm::Value * column2Ptr = createPointerValue(table1_column2.front(), column2Ty);

    auto elem3Td = std::make_unique<Integer>(1);
    llvm::Type * column3Ty = llvm::ArrayType::get(elem3Td->getLLVMType(), table1_column3.size());
    llvm::Value * column3Ptr = createPointerValue(table1_column3.front(), column3Ty);
    //] end loop

    cg_size_t table1Size = table1_column1.size();

#ifdef __APPLE__
    cg_size_t sizeNull(0ull);
#else
    cg_size_t sizeNull(0ul);
#endif

    LoopGen scanLeftLoop(funcGen, {{"index1", sizeNull}});
    cg_size_t index1(scanLeftLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLeftLoop);

        Functions::genPrintfCall("scan iteration: %lu\n", index1);

        //[ loop start
        llvm::Value * elem1Ptr = codeGen->CreateGEP(column1Ty, column1Ptr, {sizeNull, index1});
        auto elem1 = Value::load(elem1Ptr, elem1Td->type);

        llvm::Value * elem2Ptr = codeGen->CreateGEP(column2Ty, column2Ptr, {sizeNull, index1});
        auto elem2 = Value::load(elem2Ptr, elem2Td->type);

        llvm::Value * elem3Ptr = codeGen->CreateGEP(column3Ty, column3Ptr, {sizeNull, index1});
        auto elem3 = Value::load(elem3Ptr, elem3Td->type);
        //] end loop
/*
        genPrintSqlValue(elem1);
        genPrintSqlValue(elem2);
        genPrintSqlValue(elem3);
*/
        //[ join consume left start

        cg_hash_t h = elem2->hash();
        for (size_t i = 1; i < 2; ++i) { // use join attributes vector
            h = genHashCombine(h, elem3->hash());
        }

        std::vector<std::unique_ptr<Value>> values;
        values.push_back(std::move(elem1));
        values.push_back(std::move(elem2));
        values.push_back(std::move(elem3));
        SqlTuple tuple(std::move(values));

        genPrintSqlTuple(tuple);

        listNodeTy = getListNodeTy( tuple.getType() );
        genAppendEntryToList(memoryPool, listHeaderPtr, listNodeTy, h, tuple);

        //] join consume left end
    }
    cg_size_t nextIndex1 = index1 + 1ul;
    scanLeftLoop.loopDone(nextIndex1 < table1Size, {nextIndex1});
    //] scan left (produce) end

    // build hashtable
    auto headerData = genListGetHeaderData(listHeaderPtr);
    cg_voidptr_t joinTable = genStaticHashtableCreateCall(headerData.first, headerData.second);
    Functions::genPrintfCall("joinTable: %p\n", joinTable);

    // debug:
    cg_hash_t testh(6026813213711096026ul);
    genStaticHashtableIter(joinTable, testh, &printResult);

    Functions::genPrintfCall("============ PROBE ============\n");

    //[ scan right (produce) start
    //[ loop start
    auto elem4Td = Text::castString("right1", getTextTy()); // store
    llvm::Type * column4Ty = llvm::ArrayType::get(elem4Td->getLLVMType(), table2_column1.size()); // store
    llvm::Value * column4Ptr = createPointerValue(table2_column1.front(), column4Ty); // store

    auto elem5Td = std::make_unique<Integer>(1);
    llvm::Type * column5Ty = llvm::ArrayType::get(elem5Td->getLLVMType(), table2_column2.size());
    llvm::Value * column5Ptr = createPointerValue(table2_column2.front(), column5Ty);

    auto elem6Td = std::make_unique<Integer>(1);
    llvm::Type * column6Ty = llvm::ArrayType::get(elem6Td->getLLVMType(), table2_column3.size());
    llvm::Value * column6Ptr = createPointerValue(table2_column3.front(), column3Ty);
    //] end loop

    cg_size_t table2Size = table2_column1.size();

    LoopGen scanRightLoop(funcGen, {{"index2", sizeNull}});
    cg_size_t index2(scanRightLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanRightLoop);

        Functions::genPrintfCall("scan2 iteration: %lu\n", index2);

        //[ loop start
        llvm::Value * elem4Ptr = codeGen->CreateGEP(column4Ty, column4Ptr, {sizeNull, index2});
        auto elem4 = Value::load(elem4Ptr, elem1Td->type);

        llvm::Value * elem5Ptr = codeGen->CreateGEP(column5Ty, column5Ptr, {sizeNull, index2});
        auto elem5 = Value::load(elem5Ptr, elem2Td->type);

        llvm::Value * elem6Ptr = codeGen->CreateGEP(column3Ty, column6Ptr, {sizeNull, index2});
        auto elem6 = Value::load(elem6Ptr, elem3Td->type);
        //] end loop

        Utils::genPrintValue(*elem4);
        Functions::genPrintfCall(", ");
        Utils::genPrintValue(*elem5);
        Functions::genPrintfCall(", ");
        Utils::genPrintValue(*elem6);
        Functions::genPrintfCall("\n");

        rightProduced.push_back(elem4.get());
        rightProduced.push_back(elem5.get());
        rightProduced.push_back(elem6.get());

        //[ join consume right start
        // consume(std::vector<SqlValue *>

        cg_hash_t h = elem5->hash();
        for (size_t i = 1; i < 2; ++i) { // use join attributes vector
            h = genHashCombine(h, elem6->hash());
        }

        {
            cg_voidptr_t first = genStaticHashtableLookupCall(joinTable, h);
            llvm::Type * i8Ptr = llvm::PointerType::getInt8PtrTy(codeGen.getLLVMContext() );

//    Functions::genPrintfCall("lookup result: %p\n", first);

            // iterate over the all members of the bucket
            LoopGen bucketIter(funcGen, !first.isNullPtr(), {{"current", first}});
            cg_voidptr_t current(bucketIter.getLoopVar(0));
            {
                LoopBodyGen bodyGen(bucketIter);

                // the second field of the "Node"-struct contains the hashcode
                cg_voidptr_t hp = current + sizeof(void *);
//        Functions::genPrintfCall("hashcode ptr: %p\n", hp);

                cg_hash_t currentHash(codeGen->CreateLoad(cg_hash_t::getType(), hp));
//        Functions::genPrintfCall("hashcode: %lu\n", currentHash);

                IfGen compareHash(funcGen, h == currentHash);
                {
                    // elementHandler is supposed to generate code that handles the current match
                    //doProbe(current, table1_column1, table1_column2, table1_column3);
                }
                compareHash.EndIf();
            }
            // the first field of the struct represents the "next"-pointer
            cg_voidptr_t next(codeGen->CreateLoad(i8Ptr, current));
            bucketIter.loopDone(!next.isNullPtr(), {next});
        }


        //] join consume right end
    }
    cg_size_t nextIndex2 = index2 + 1ul;
    scanRightLoop.loopDone(nextIndex2 < table2Size, {nextIndex2});
    //] scan right (produce) end
    //] join produce end

    //[ join epilog start
    genStaticHashtableFreeCall(joinTable);
    genMemoryPoolFreeCall(memoryPool);
    //] join epilog end

    return funcGen.getFunction();
}

/*TEST(JoinTest, Join1Test) {
    ModuleGen moduleGen("JoinTestModule");
    std::vector<llvm::GenericValue> args;

    Vector *table1_column1 = new Vector(16,9);
    Vector *table1_column2 = new Vector(4,9);
    Vector *table1_column3 = new Vector(4,9);
    Vector *table2_column1 = new Vector(16,6);
    Vector *table2_column2 = new Vector(4,6);
    Vector *table2_column3 = new Vector(4,6);

    std::vector<value_op_t> persistentStore;
    llvm::Function * joinTest = genJoinTest(persistentStore, *table1_column1, *table1_column2, *table1_column3, *table2_column1, *table2_column2, *table2_column3);
    llvm::GenericValue resultJoinTest = QueryExecutor::executeFunction(joinTest,args);
    ASSERT_NE(resultJoinTest.IntVal.getZExtValue(),0);
}*/

void executeJoinTest() {
    ModuleGen moduleGen("JoinTestModule");
    std::vector<llvm::GenericValue> args;

    std::unique_ptr<Vector> table1_column1 = std::make_unique<Vector>(16);
    std::unique_ptr<Vector> table1_column2 = std::make_unique<Vector>(4);
    std::unique_ptr<Vector> table1_column3 = std::make_unique<Vector>(4);
    std::unique_ptr<Vector> table2_column1 = std::make_unique<Vector>(16);
    std::unique_ptr<Vector> table2_column2 = std::make_unique<Vector>(4);
    std::unique_ptr<Vector> table2_column3 = std::make_unique<Vector>(4);
    std::vector<value_op_t> persistentStore;
    llvm::Function * joinTest = genJoinTest(persistentStore , *table1_column1, *table1_column2, *table1_column3, *table2_column1, *table2_column2, *table2_column3);
    llvm::GenericValue resultJoinTest = QueryExecutor::executeFunction(joinTest,args);
    std::cout << "test: passed: " << resultJoinTest.IntVal.getZExtValue() << "\n";
}

void join_test() {
    executeJoinTest();
}