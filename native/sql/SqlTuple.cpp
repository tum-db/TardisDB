#include "SqlTuple.hpp"

#include <functional>

#include "foundations/LegacyTypes.hpp"
//#include "foundations/utils.hpp"
#include "sql/SqlType.hpp"

using Sql::SqlType;

namespace Native {
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
    std::tie(typeName, structTy) = constructStructTy(types);
}

std::pair<std::string, llvm::StructType *> SqlTuple::constructStructTy(const std::vector<SqlType> & types)
{
    auto & typeCache = getThreadLocalCodeGen().getTypeCache();

    std::string name = constructTypeName(types);
    llvm::StructType * type = static_cast<llvm::StructType *>(typeCache.get(name));

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

    return std::make_pair(name, type);
}

std::string SqlTuple::constructTypeName(const std::vector<SqlType> & types)
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
    size_t size = dataLayout.getTypeAllocSize(structTy);
    return size;
}

void SqlTuple::store(void * ptr)
{
    // https://stackoverflow.com/questions/47949969/llvm-get-the-offset-of-inside-struct

    auto & codeGen = getThreadLocalCodeGen();
    auto & dataLayout = codeGen.getDefaultDataLayout();
    auto structLayout = dataLayout.getStructLayout(structTy);

    uint8_t * bytePtr = static_cast<uint8_t *>(ptr);

    // store each member value
    unsigned i = 0;
    for (auto & sqlVal : values) {
        size_t offset = structLayout->getElementOffset(i);
        sqlVal->store(bytePtr + offset);
        i += 1;
    }
}

std::unique_ptr<SqlTuple> SqlTuple::load(const void * ptr, const std::vector<SqlType> & types)
{
    std::string typeName;
    llvm::StructType * structTy;
    std::tie(typeName, structTy) = constructStructTy(types);

    auto & codeGen = getThreadLocalCodeGen();
    auto & dataLayout = codeGen.getDefaultDataLayout();
    auto structLayout = dataLayout.getStructLayout(structTy);

    std::vector<value_op_t> values;
    const uint8_t * bytePtr = static_cast<const uint8_t *>(ptr);

    // load each member value
    unsigned i = 0;
    for (SqlType type : types) {
        size_t offset = structLayout->getElementOffset(i);
        const void * valuePtr = bytePtr + offset;
        values.push_back( Value::load(valuePtr, type) );
        i += 1;
    }

    return std::unique_ptr<SqlTuple>( new SqlTuple( std::move(values) ) );
}

hash_t SqlTuple::hash()
{
    assert(values.size() > 0);

    hash_t seed = values.front()->hash();

    // combine the hashes of all members
    auto it = std::next(values.begin());
    for (; it != values.end(); ++it) {
        auto & sqlValue = *it;
        hash_t h = sqlValue->hash();
        HashUtils::hash_combine(seed, h);
    }

    return seed;
}

} // end namespace Sql
} // end namespace Sql
