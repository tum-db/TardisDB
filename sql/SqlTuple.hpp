
#ifndef SQLTUPLE_H
#define SQLTUPLE_H

#include <set>
#include <vector>

#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include "codegen/CodeGen.hpp"
#include "sql/SqlValues.hpp"

namespace Sql {

/// SqlTuple is a simple storage class for multiple SqlValues
class SqlTuple {
public:
    std::vector<value_op_t> values;

    SqlTuple(std::vector<value_op_t> && values);

    SqlTuple & operator =(const SqlTuple & rhs) = delete;

    static std::unique_ptr<SqlTuple> load(llvm::Value * ptr, const std::vector<SqlType> & types);

    void store(llvm::Value * ptr);

    size_t getSize();

    cg_hash_t hash();

    std::vector<value_op_t> split() { return std::move(values); }

    llvm::Type * getType();

    static llvm::Type * getType(const std::vector<SqlType> & tds);

private:
    std::string name;

    static std::string getTypeName(const std::vector<SqlType> & tds);
};

void genPrintSqlTuple(SqlTuple & sqlTuple);

cg_hash_t genHashCombine(cg_hash_t h, cg_hash_t k);

} // end namespace Sql

#endif // SQLTUPLE_H
