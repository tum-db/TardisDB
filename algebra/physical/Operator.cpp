//
// Created by josef on 30.12.16.
//

#include "Operator.hpp"

#include "foundations/exceptions.hpp"

namespace Algebra {
namespace Physical {

//-----------------------------------------------------------------------------
// Operator

Operator::Operator(QueryContext & context, const iu_set_t & required) :
        codeGen(context.codeGen),
        context(context),
        _required(required)
{ }

Operator::~Operator()
{ }

QueryContext & Operator::getContext() const
{
    return context;
}

Operator * Operator::getParent() const
{
    return parent;
}

void Operator::setParent(Operator * parent)
{
    this->parent = parent;
}

const iu_set_t & Operator::getRequired()
{
    return _required;
}

//-----------------------------------------------------------------------------
// NullaryOperator

NullaryOperator::NullaryOperator(QueryContext & context, const iu_set_t & required) :
        Operator(context, required)
{ }

NullaryOperator::~NullaryOperator()
{ }

void NullaryOperator::consume(const iu_value_mapping_t & values, const Operator & src)
{
    throw InvalidOperationException("NullaryOperator can't consume anything");
}

size_t NullaryOperator::arity() const
{
    return 0;
}

//-----------------------------------------------------------------------------
// UnaryOperator
UnaryOperator::UnaryOperator(std::unique_ptr<Operator> input, const iu_set_t & required) :
        Operator(input->getContext(), required),
        child(std::move(input))
{
    child->setParent(this);
}

UnaryOperator::~UnaryOperator()
{ }

size_t UnaryOperator::arity() const
{
    return 1;
}

//-----------------------------------------------------------------------------
// BinaryOperator
BinaryOperator::BinaryOperator(std::unique_ptr<Operator> leftInput, std::unique_ptr<Operator> rightInput,
                               const iu_set_t & required) :
        Operator(leftInput->getContext(), required),
        leftChild(std::move(leftInput)),
        rightChild(std::move(rightInput))
{
    leftChild->setParent(this);
    rightChild->setParent(this);
}

BinaryOperator::~BinaryOperator()
{ }

void BinaryOperator::consume(const iu_value_mapping_t & values, const Operator & src)
{
    if (&src == leftChild.get()) {
        consumeLeft(values);
    } else {
        consumeRight(values);
    }
}

size_t BinaryOperator::arity() const
{
    return 2;
}

} // end namespace Physical
} // end namespace Algebra
