//
// Created by josef on 30.12.16.
//

#include "Operator.hpp"

#include "foundations/exceptions.hpp"

namespace Algebra {
namespace Physical {

//-----------------------------------------------------------------------------
// Operator

Operator::Operator(logical_operator_op_t logicalOperator) :
        _codeGen(logicalOperator->getContext().codeGen),
        _context(logicalOperator->getContext()),
        _logicalOperator(std::move(logicalOperator))
{ }

Operator::~Operator()
{ }

QueryContext & Operator::getContext() const
{
    return _context;
}

Operator * Operator::getParent() const
{
    return _parent;
}

void Operator::setParent(Operator * parent)
{
    this->_parent = parent;
}

const iu_set_t & Operator::getRequired()
{
    return _logicalOperator->getRequired();
}

//-----------------------------------------------------------------------------
// NullaryOperator

NullaryOperator::NullaryOperator(logical_operator_op_t logicalOperator) :
        Operator(std::move(logicalOperator))
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
UnaryOperator::UnaryOperator(logical_operator_op_t logicalOperator, std::unique_ptr<Operator> input) :
        Operator(std::move(logicalOperator)),
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
BinaryOperator::BinaryOperator(logical_operator_op_t logicalOperator, std::unique_ptr<Operator> leftInput,
                               std::unique_ptr<Operator> rightInput) :
        Operator(std::move(logicalOperator)),
        _leftChild(std::move(leftInput)),
        _rightChild(std::move(rightInput))
{
    _leftChild->setParent(this);
    _rightChild->setParent(this);
}

BinaryOperator::~BinaryOperator()
{ }

void BinaryOperator::consume(const iu_value_mapping_t & values, const Operator & src)
{
    if (&src == _leftChild.get()) {
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
