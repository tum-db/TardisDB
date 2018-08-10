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
    child->produce();
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
}

} // end namespace Physical
} // end namespace Algebra
