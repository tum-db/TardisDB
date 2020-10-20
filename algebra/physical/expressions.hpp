
#pragma once

#include <memory>
#include <iostream>
#include <stack>
#include <vector>
#include <string>
#include <unordered_map>

#include "algebra/logical/expressions.hpp"
#include "algebra/physical/Operator.hpp"
#include "codegen/CodeGen.hpp"
#include "semanticAnalyser/logicalAlgebra/InformationUnit.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlOperators.hpp"

namespace Algebra {
namespace Physical {
namespace Expressions {

/// code generation expression base class
class Expression {
public:
    Expression(Sql::SqlType type) :
            _type(type)
    { }

    virtual ~Expression() { }

    virtual Sql::value_op_t evaluate(const iu_value_mapping_t & values) = 0;

    virtual Sql::SqlType getType() const final { return _type; }

    virtual Logical::Expressions::Expression & logical() const final { return *_logical; }

private:
    const Sql::SqlType _type;
    Logical::Expressions::exp_op_t _logical; // TODO
};

using exp_op_t = std::unique_ptr<Expression>;
using physical_exp_op_t = exp_op_t;

class UnaryOperator : public Expression {
public:
    exp_op_t _child;

    ~UnaryOperator() override { }

protected:
    UnaryOperator(Sql::SqlType type, exp_op_t child) :
            Expression(type),
            _child(std::move(child))
    { }
};

class Cast : public UnaryOperator {
public:
    Cast(exp_op_t child, Sql::SqlType destType) :
            UnaryOperator(destType, std::move(child))
    { }

    ~Cast() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto value = _child->evaluate(values);
        return Sql::Operators::sqlCast(*value, getType());
    }
};

class Not : public UnaryOperator {
public:
    Not(exp_op_t child) :
            UnaryOperator(child->getType(), std::move(child))
    {
        assert(child->getType().typeID == Sql::SqlType::TypeID::BoolID);
    }

    ~Not() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto value = _child->evaluate(values);
        return Sql::Operators::sqlNot(*value);
    }
};

class BinaryOperator : public Expression {
public:
    exp_op_t _left;
    exp_op_t _right;

    ~BinaryOperator() override { }

protected:
    BinaryOperator(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            Expression(type),
            _left(std::move(left)), _right(std::move(right))
    { }
};

class And : public BinaryOperator {
public:
    And(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right))
    {
        // TODO null values?
        assert(type.typeID == Sql::SqlType::TypeID::BoolID && type.typeID == Sql::SqlType::TypeID::BoolID);
    }

     ~And() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlAnd(*lhs, *rhs);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

class Or : public BinaryOperator {
public:
    Or(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right))
    {
        // TODO null values?
        assert(type.typeID == Sql::SqlType::TypeID::BoolID && type.typeID == Sql::SqlType::TypeID::BoolID);
    }

     ~Or() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlOr(*lhs, *rhs);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

class Addition : public BinaryOperator {
public:
    Addition(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right))
    {
        assert(type.typeID == _left->getType().typeID && type.typeID == _right->getType().typeID);
    }

     ~Addition() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlAdd(*lhs, *rhs);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

class Subtraction : public BinaryOperator {
public:
    Subtraction(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right))
    {
        assert(type.typeID == _left->getType().typeID && type.typeID == _right->getType().typeID);
    }

    ~Subtraction() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlSub(*lhs, *rhs);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

class Multiplication : public BinaryOperator {
public:
    Multiplication(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right))
    {
        assert(type.typeID == _left->getType().typeID && type.typeID == _right->getType().typeID);
    }

    ~Multiplication() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlMul(*lhs, *rhs);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

class Division : public BinaryOperator {
public:
    Division(Sql::SqlType type, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right))
    {
        assert(type.typeID == _left->getType().typeID && type.typeID == _right->getType().typeID);
    }

    ~Division() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlDiv(*lhs, *rhs);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

using ComparisonMode = Logical::Expressions::ComparisonMode;

class Comparison : public BinaryOperator {
public:
    ComparisonMode _mode;

    Comparison(Sql::SqlType type, ComparisonMode mode, exp_op_t left, exp_op_t right) :
            BinaryOperator(type, std::move(left), std::move(right)),
            _mode(mode)
    { }

    ~Comparison() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        auto lhs = _left->evaluate(values);
        auto rhs = _right->evaluate(values);
        auto result = Sql::Operators::sqlCompare(*lhs, *rhs, _mode);
        assert(Sql::equals(result->type, getType(), Sql::SqlTypeEqualsMode::Full));
        return result;
    }
};

class Identifier : public Expression {
public:
    iu_p_t _iu;

    Identifier(iu_p_t iu) :
            Expression(iu->sqlType),
            _iu(iu)
    { }

    ~Identifier() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        return values.at(_iu)->clone();
    }
};

class Constant : public Expression {
public:
    Sql::value_op_t _value;

    Constant(Sql::value_op_t value) :
            Expression(value->type),
            _value(std::move(value))
    { }

    ~Constant() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        return _value->clone();
    }
};

class NullConstant : public Expression {
public:
    NullConstant() :
            Expression(Sql::getUnknownTy())
    { }

    ~NullConstant() override { }

    Sql::value_op_t evaluate(const iu_value_mapping_t & values) override
    {
        // "null" has no specific type
        return Sql::UnknownValue::create();
    }
};

} // end namespace Expressions
} // end namespace Physical
} // end namespace Algebra
