
#include "expressions.hpp"

namespace Algebra {
namespace Logical {
namespace Expressions {

void Cast::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Addition::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Subtraction::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Multiplication::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Division::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Comparison::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Identifier::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void Constant::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

void NullConstant::accept(ExpressionVisitor & visitor)
{
    visitor.visit(*this);
}

} // end namespace Expressions
} // end namespace Logical
} // end namespace Algebra
