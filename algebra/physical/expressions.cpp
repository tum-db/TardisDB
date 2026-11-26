#include "algebra/physical/expressions.hpp"

#include "codegen/LLVMCompat.hpp"

#include <random>

namespace Algebra {
namespace Physical {
namespace Expressions {

/// \returns A double precision in [0, 1) of the generator of the thread
static double randomDouble()
{
    thread_local std::mt19937_64 generator{std::random_device{}()};
    return std::uniform_real_distribution<double>(0.0, 1.0)(generator);
}

Sql::value_op_t Random::evaluate(const iu_value_mapping_t & values)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::FunctionType * type = llvm::FunctionType::get(codeGen->getDoubleTy(), false);
    return Sql::Double::fromRawValues({codeGen.CreateCall(&randomDouble, type, std::vector<llvm::Value *>())});
}

} // end namespace Expressions
} // end namespace Physical
} // end namespace Algebra
