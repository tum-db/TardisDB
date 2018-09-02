//
// Created by josef on 03.01.17.
//

#include "algebra/physical/Print.hpp"
#include "foundations/utils.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

Print::Print(std::unique_ptr<Operator> input, const std::vector<iu_p_t> & selection) :
        UnaryOperator(std::move(input), iu_set_t()),
        selection(selection)
{
    // "selection" represents the required attributes of the print operator
    for (iu_p_t ciu : selection) {
        _required.insert(ciu);
    }
}

Print::~Print()
{ }

void Print::produce()
{
    tupleCountPtr = codeGen->CreateAlloca(cg_size_t::getType());
    codeGen->CreateStore(cg_size_t(0ul), tupleCountPtr);

    child->produce();

    cg_size_t tupleCnt(codeGen->CreateLoad(tupleCountPtr));
    IfGen check(tupleCnt == cg_size_t(0ul));
    {
        Functions::genPrintfCall("Empty result set\n");
    }
    check.Else();
    {
        Functions::genPrintfCall("Produced %lu tuples\n", tupleCnt);
    }
    check.EndIf();
}

void Print::consume(const iu_value_mapping_t & values, const Operator & src)
{
    bool first = true;
    for (iu_p_t iu : selection) {
        if (!first) {
            Functions::genPrintfCall("\t");
        }
        first = false;

        // there should be still some room for optimisation by combining the printf calls for all values
        Sql::Utils::genPrintValue(*values.at(iu));
    }
    Functions::genPrintfCall("\n");

    // increment tuple counter
    cg_size_t prevCnt(codeGen->CreateLoad(tupleCountPtr));
    codeGen->CreateStore(prevCnt + 1ul, tupleCountPtr);
}

} // end namespace Physical
} // end namespace Algebra
