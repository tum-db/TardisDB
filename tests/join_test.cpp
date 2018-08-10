
#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/StaticHashtable.hpp"
#include "foundations/MemoryPool.hpp"
#include "foundations/Types.hpp"
#include "foundations/utils.hpp"
#include "sql/SqlTypes.hpp"

// This test also represents the draft for the HashJoin operator

// table 1:
static std::vector<Varchar<20>> table1_column1 = {
        Varchar<20>::castString("left1", 5),
        Varchar<20>::castString("left2", 5),
        Varchar<20>::castString("left3", 5),
        Varchar<20>::castString("left4", 5),
        Varchar<20>::castString("left5", 5),
        Varchar<20>::castString("left6", 5),
        Varchar<20>::castString("left7", 5),
        Varchar<20>::castString("left8", 5),
        Varchar<20>::castString("left9", 5)
};

static std::vector<Integer> table1_column2 = { 1, 2, 2, 3, 4, 5,  12, 13, 12 };
static std::vector<Integer> table1_column3 = { 6, 7, 7, 8, 9, 10, 12, 13, 12 };

// table 2:
static std::vector<Varchar<20>> table2_column1 = {
        Varchar<20>::castString("right1", 6),
        Varchar<20>::castString("right2", 6),
        Varchar<20>::castString("right3", 6),
        Varchar<20>::castString("right4", 6),
        Varchar<20>::castString("right5", 6),
        Varchar<20>::castString("right6", 6),
};

static std::vector<Integer> table2_column2 = { 1, 2, 2, 3, 3, 5 };
static std::vector<Integer> table2_column3 = { 6, 7, 7, 8, 8, 11 };

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

static std::vector<SqlValue *> rightProduced;

static llvm::Type * listNodeTy;

static void doProbe(cg_voidptr_t p)
{
    Functions::genPrintfCall("===\ndoProbe() iter node: %p\n===\n", p);

    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    c_td_vec_t tds {
            &table1_column1.front().typeDescriptor(),
            &table1_column2.front().typeDescriptor(),
            &table1_column3.front().typeDescriptor()
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

    IfGen check(funcGen, all);
    {
        Functions::genPrintfCall("RESULT: ");
        genPrintSqlValue(*(t->values[0]));
        Functions::genPrintfCall(", ");
        genPrintSqlValue(*rightProduced[0]);
        Functions::genPrintfCall("\n");


        // parent.consume(SqlValue's)
    }
    check.EndIf();
}


llvm::Function * join_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "query", funcTy);

    //std::vector join_attr_indices { 1, 2 };

    //[ join produce start
    cg_voidptr_t memoryPool = genMemoryPoolCreateCall();
    llvm::Value * listHeaderPtr = genCreateListHeader(memoryPool);

    //[ scan left (produce) start
    //[ loop start
    auto & elem1Td = table1_column1.front().typeDescriptor(); // store
    llvm::Type * column1Ty = llvm::ArrayType::get(elem1Td.getType(context), table1_column1.size()); // store
    llvm::Value * column1Ptr = createPointerValue(&table1_column1.front(), column1Ty); // store

    auto & elem2Td = table1_column2.front().typeDescriptor();
    llvm::Type * column2Ty = llvm::ArrayType::get(elem2Td.getType(context), table1_column2.size());
    llvm::Value * column2Ptr = createPointerValue(&table1_column2.front(), column2Ty);

    auto & elem3Td = table1_column3.front().typeDescriptor();
    llvm::Type * column3Ty = llvm::ArrayType::get(elem3Td.getType(context), table1_column3.size());
    llvm::Value * column3Ptr = createPointerValue(&table1_column3.front(), column3Ty);
    //] end loop

    cg_size_t table1Size = table1_column1.size();

    LoopGen scanLeftLoop(funcGen, {{"index1", cg_size_t(0ul)}});
    cg_size_t index1(scanLeftLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLeftLoop);

        Functions::genPrintfCall("scan iteration: %lu\n", index1);

        //[ loop start
        llvm::Value * elem1Ptr = codeGen->CreateGEP(column1Ty, column1Ptr, {cg_size_t(0ul), index1});
        auto elem1 = SqlValue::load(elem1Ptr, elem1Td);

        llvm::Value * elem2Ptr = codeGen->CreateGEP(column2Ty, column2Ptr, {cg_size_t(0ul), index1});
        auto elem2 = SqlValue::load(elem2Ptr, elem2Td);

        llvm::Value * elem3Ptr = codeGen->CreateGEP(column3Ty, column3Ptr, {cg_size_t(0ul), index1});
        auto elem3 = SqlValue::load(elem3Ptr, elem3Td);
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

        std::vector<std::unique_ptr<SqlValue>> values;
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
    auto & elem4Td = table2_column1.front().typeDescriptor(); // store
    llvm::Type * column4Ty = llvm::ArrayType::get(elem4Td.getType(context), table2_column1.size()); // store
    llvm::Value * column4Ptr = createPointerValue(&table2_column1.front(), column4Ty); // store

    auto & elem5Td = table2_column2.front().typeDescriptor();
    llvm::Type * column5Ty = llvm::ArrayType::get(elem5Td.getType(context), table2_column2.size());
    llvm::Value * column5Ptr = createPointerValue(&table2_column2.front(), column5Ty);

    auto & elem6Td = table2_column3.front().typeDescriptor();
    llvm::Type * column6Ty = llvm::ArrayType::get(elem6Td.getType(context), table2_column3.size());
    llvm::Value * column6Ptr = createPointerValue(&table2_column3.front(), column3Ty);
    //] end loop

    cg_size_t table2Size = table2_column1.size();

    LoopGen scanRightLoop(funcGen, {{"index2", cg_size_t(0ul)}});
    cg_size_t index2(scanRightLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanRightLoop);

        Functions::genPrintfCall("scan2 iteration: %lu\n", index2);

        //[ loop start
        llvm::Value * elem4Ptr = codeGen->CreateGEP(column4Ty, column4Ptr, {cg_size_t(0ul), index2});
        auto elem4 = SqlValue::load(elem4Ptr, elem1Td);

        llvm::Value * elem5Ptr = codeGen->CreateGEP(column5Ty, column5Ptr, {cg_size_t(0ul), index2});
        auto elem5 = SqlValue::load(elem5Ptr, elem2Td);

        llvm::Value * elem6Ptr = codeGen->CreateGEP(column3Ty, column6Ptr, {cg_size_t(0ul), index2});
        auto elem6 = SqlValue::load(elem6Ptr, elem3Td);
        //] end loop

        genPrintSqlValue(*elem4);
        Functions::genPrintfCall(", ");
        genPrintSqlValue(*elem5);
        Functions::genPrintfCall(", ");
        genPrintSqlValue(*elem6);
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

        genStaticHashtableIter(joinTable, h, &doProbe);

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
