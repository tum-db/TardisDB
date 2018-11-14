#pragma once

#include <set>
#include <vector>

#include <llvm/IR/DerivedTypes.h>

#include "native/sql/SqlValues.hpp"

namespace Native {
namespace Sql {

/// SqlTuple is a simple storage class for multiple SqlValues
class SqlTuple {
public:
    std::vector<value_op_t> values;

    SqlTuple(std::vector<value_op_t> && values);

    SqlTuple & operator =(const SqlTuple & rhs) = delete;

    static std::unique_ptr<SqlTuple> load(const void * ptr, const std::vector<SqlType> & types);

    void store(void * ptr);

    size_t getSize();

    hash_t hash();

    std::vector<value_op_t> split() { return std::move(values); }

    static std::pair<std::string, llvm::StructType *> constructStructTy(const std::vector<SqlType> & tds);

private:
    std::string typeName;
    llvm::StructType * structTy;

    static std::string constructTypeName(const std::vector<SqlType> & tds);
};

} // end namespace Sql
} // end namespace Native
