
#include "QueryContext.hpp"

#include "exceptions.hpp"
#include "algebra/logical/operators.hpp"

void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol)
{
    context.scope.emplace(symbol, iu);
}

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan)
{
    auto & scope = context.scope;
    for (iu_p_t iu : scan.getProduced()) {
        ci_p_t ci = getColumnInformation(iu);
        scope.emplace(ci->columnName, iu);
    }
}

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix)
{
    auto & scope = context.scope;
    for (iu_p_t iu : scan.getProduced()) {
        ci_p_t ci = getColumnInformation(iu);
        scope.emplace(prefix + "." + ci->columnName, iu);
    }
}

iu_p_t lookup(QueryContext & context, const std::string & symbol)
{
    auto & scope = context.scope;
    auto it = scope.find(symbol);
    if (it == scope.end()) {
        throw ParserException("unknown identifier: " + symbol);
    }
    return it->second;
}

void genOverflowException()
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Value * ptrIntVal = codeGen->getIntN( TypeWrappers::ptrBitSize, reinterpret_cast<ptr_int_rep_t>(&overflowFlag) );
    llvm::Value * ptrVal = codeGen->CreateIntToPtr(
            ptrIntVal, llvm::PointerType::getIntNPtrTy(codeGen.getLLVMContext(), TypeWrappers::ptrBitSize) );
    llvm::Value * value = codeGen->getInt1(true);
    codeGen->CreateStore(value, ptrVal);
}

void genOverflowEvaluation()
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Value * ptrIntVal = codeGen->getIntN( TypeWrappers::ptrBitSize, reinterpret_cast<ptr_int_rep_t>(&overflowFlag) );
    llvm::Value * ptrVal = codeGen->CreateIntToPtr(
            ptrIntVal, llvm::PointerType::getIntNPtrTy(codeGen.getLLVMContext(), TypeWrappers::ptrBitSize) );
    llvm::Value * obit = codeGen->CreateLoad(ptrVal);

    IfGen overflowCheck(obit);
    {
        Functions::genPrintfCall("numerical overflow\n");
    }
}
