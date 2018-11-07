#include "SqlTuple.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/TypeBuilder.h>

#include <functional>

#include "codegen/CodeGen.hpp"
#include "foundations/LegacyTypes.hpp"
#include "sql/SqlUtils.hpp"

namespace Sql {

//-----------------------------------------------------------------------------
// SqlTuple

SqlTuple::SqlTuple(std::vector<value_op_t> && values) :
        values( std::move(values) )
{
    std::vector<SqlType> types;
    for (auto & sqlVal : this->values) {
        types.push_back(sqlVal->type);
    }
    name = getTypeName(types);
}

llvm::Type * SqlTuple::getType()
{
    auto & typeCache = getThreadLocalCodeGen().getTypeCache();

    llvm::Type * type = typeCache.get(name);

    // create the type signature
    if (type == nullptr) {
        auto & context = getThreadLocalCodeGen().getLLVMContext();

        // collect members
        std::vector<llvm::Type *> members;
        for (auto & sqlVal : values) {
            members.push_back(sqlVal->getLLVMType());
        }

        llvm::StructType * tupleTy = llvm::StructType::create(context, name);
        tupleTy->setBody(members);

        type = tupleTy;
        typeCache.add(name, type); // cache for further use
    }

    return type;
}

llvm::Type * SqlTuple::getType(const std::vector<SqlType> & types)
{
    auto & typeCache = getThreadLocalCodeGen().getTypeCache();

    std::string name = getTypeName(types);
    llvm::Type * type = typeCache.get(name);

    // create the type signature
    if (type == nullptr) {
        auto & context = getThreadLocalCodeGen().getLLVMContext();

        // collect members
        std::vector<llvm::Type *> members;
        for (SqlType sqlType : types) {
            llvm::Type * memberTy = toLLVMTy(sqlType);
            members.push_back(memberTy);
        }

        llvm::StructType * tupleTy = llvm::StructType::create(context, name);
        tupleTy->setBody(members);

        type = tupleTy;
        typeCache.add(name, type); // cache for further use
    }

    return type;
}

std::string SqlTuple::getTypeName(const std::vector<SqlType> & types)
{
    std::string name("SqlTuple<");
    bool first = true;
    for (SqlType type : types) {
        if (!first) {
            name.append(", ");
        }
        first = false;
        name.append( getName(type) );
    }
    name.append(">");
    return name;
}

size_t SqlTuple::getSize()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & dataLayout = codeGen.getCurrentModuleGen().getDataLayout();
    size_t size = dataLayout.getTypeAllocSize( getType() );
    return size;
}

void SqlTuple::store(llvm::Value * ptr)
{
    auto & codeGen = getThreadLocalCodeGen();

//    Functions::genPrintfCall("store tuple at %lu\n", ptr);

    llvm::Type * tupleTy = getType();

    // store each member value
    unsigned i = 0;
    for (auto & sqlVal : values) {
        llvm::Value * valuePtr;

        valuePtr = codeGen->CreateStructGEP(tupleTy, ptr, i);

//        Functions::genPrintfCall("store tuple elem at %lu - size: %lu\n", valuePtr, cg_size_t(sqlVal->getSize()));
        sqlVal->store(valuePtr);

        i += 1;
    }
}

std::unique_ptr<SqlTuple> SqlTuple::load(llvm::Value * ptr, const std::vector<SqlType> & types)
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * structTy = getType(types);
    std::vector<value_op_t> values;

    // load each member value
    unsigned i = 0;
    for (SqlType type : types) {
        llvm::Value * valuePtr = codeGen->CreateStructGEP(structTy, ptr, i);
        values.push_back( Value::load(valuePtr, type) );

        i += 1;
    }

    return std::unique_ptr<SqlTuple>( new SqlTuple( std::move(values) ) );
}

cg_hash_t SqlTuple::hash()
{
    assert(values.size() > 0);

    cg_hash_t seed = values.front()->hash();

    // combine the hashes of all members
    auto it = std::next(values.begin());
    for (; it != values.end(); ++it) {
        auto & sqlValue = *it;
        cg_hash_t h = sqlValue->hash();
        seed = genHashCombine(seed, h);
    }

    return seed;
}

//-----------------------------------------------------------------------------
// utils

// only for testing
void genPrintSqlTuple(SqlTuple & sqlTuple)
{
    Functions::genPrintfCall("(");
    bool first = true;
    for (auto & sqlValue : sqlTuple.values) {
        if (!first) {
            Functions::genPrintfCall(", ");
        }
        first = false;
        Utils::genPrintValue(*sqlValue);
    }
    Functions::genPrintfCall(")\n");
}

// source: boost
cg_hash_t genHashCombine(cg_hash_t h, cg_hash_t k)
{
    cg_u64_t offset(0xe6546b64ul);
    cg_u64_t m(0xc6a4a7935bd1e995ul);

    k = k*m;
    k = k ^ (k >> 47ul);
    k = k*m;

    h = h ^ k;
    h = h*m;

    h = h + offset;
    return h;
}

} // end namespace Sql
