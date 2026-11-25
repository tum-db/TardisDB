#pragma once

#include <memory>
#include <iostream>
#include <stack>
#include <vector>
#include <string>
#include <unordered_map>

#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/InformationUnit.hpp"
#include "native/sql/Array.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlCasts.hpp"
#include "sql/SqlOperators.hpp"
#include "utils/general.hpp"

namespace Algebra {
namespace Logical {
namespace Expressions {

class ExpressionVisitor;

class Expression {
public:
    Expression() { }

    virtual ~Expression() { }

    virtual Sql::SqlType getType() const = 0;

    virtual void accept(ExpressionVisitor & visitor) = 0;

// TODO
//    virtual iu_set_t computeRequired() const final;
};

using exp_op_t = std::unique_ptr<Expression>;
using logical_exp_op_t = exp_op_t;

/// \returns The type, nullable as the other (a cast keeps whether its input may be NULL)
inline Sql::SqlType withNullability(Sql::SqlType type, Sql::SqlType other)
{
    type.nullable = other.nullable;
    return type;
}

class UnaryOperator : public Expression {
public:
    ~UnaryOperator() override { }

    Expression & getChild() const { return *_child; }

protected:
    exp_op_t _child;

    UnaryOperator(exp_op_t child) :
            _child(std::move(child))
    { }
};

class Cast : public UnaryOperator {
public:
    Sql::SqlType _destType;
    Sql::Casts::Context _context;

    Cast(exp_op_t child, Sql::SqlType destType, Sql::Casts::Context context = Sql::Casts::Context::Explicit) :
            UnaryOperator(std::move(child)),
            _destType(destType),
            _context(context)
    { }

    ~Cast() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _destType;
    }
};

/// The expression in the destination type (nullable as the expression): itself if it is of that type,
/// else a cast of the cast table of its type. Throws if its type does not cast to the destination in
/// the context. An expression of no type (NULL) stays as it is.
inline exp_op_t castIfNeeded(exp_op_t expression, Sql::SqlType destination, Sql::Casts::Context context = Sql::Casts::Context::Implicit)
{
    Sql::SqlType type = expression->getType();
    if (type.typeID == Sql::SqlType::TypeID::UnknownID || Sql::equals(type, destination, Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return expression;
    }
    if (!Sql::Casts::canCast(type, destination, context)) {
        throw ArithmeticException("cannot cast type " + Sql::Casts::typeName(type) + " to " + Sql::Casts::typeName(destination));
    }
    return std::make_unique<Cast>(std::move(expression), withNullability(destination, type), context);
}

/// Both operands in their common type (see Sql::Casts::commonType), if they have one; two numerics
/// keep their scales for the arithmetic
inline void castToCommonType(exp_op_t & left, exp_op_t & right, bool keepNumerics = false)
{
    Sql::SqlType l = left->getType(), r = right->getType(), common;
    if (keepNumerics && l.typeID == Sql::SqlType::TypeID::NumericID && r.typeID == Sql::SqlType::TypeID::NumericID) return;
    if (!Sql::Casts::commonType(l, r, common)) return;
    left = castIfNeeded(std::move(left), common);
    right = castIfNeeded(std::move(right), common);
}

class Not : public UnaryOperator {
public:
    Not(exp_op_t child) :
            UnaryOperator(std::move(child))
    {
        assert(_child->getType().typeID == Sql::SqlType::TypeID::BoolID);
    }

    ~Not() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _child->getType();
    }
};

/// IS NULL, or IS NOT NULL if negated: true or false, never NULL
class IsNull : public UnaryOperator {
public:
    bool _negated;

    IsNull(exp_op_t child, bool negated) :
            UnaryOperator(std::move(child)),
            _negated(negated)
    { }

    ~IsNull() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return Sql::getBoolTy(false);
    }
};

class BinaryOperator : public Expression {
public:
    ~BinaryOperator() override { }

    Expression & getLeftChild() const { return *_left; }
    Expression & getRightChild() const { return *_right; }

protected:
    exp_op_t _left;
    exp_op_t _right;

    BinaryOperator(exp_op_t left, exp_op_t right) :
            _left(std::move(left)), _right(std::move(right))
    { }
};

class And : public BinaryOperator {
public:
    And(exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right))
    {
        if (_left->getType().typeID != Sql::SqlType::TypeID::BoolID ||
            _right->getType().typeID != Sql::SqlType::TypeID::BoolID)
        {
            throw ArithmeticException("'and' requires boolean arguments");
        }
        _type = Sql::getBoolTy(_left->getType().nullable || _right->getType().nullable);
    }

    ~And() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

class Or : public BinaryOperator {
public:
    Or(exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right))
    {
        if (_left->getType().typeID != Sql::SqlType::TypeID::BoolID ||
            _right->getType().typeID != Sql::SqlType::TypeID::BoolID)
        {
            throw ArithmeticException("'or' requires boolean arguments");
        }
        _type = Sql::getBoolTy(_left->getType().nullable || _right->getType().nullable);
    }

    ~Or() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

class Addition : public BinaryOperator {
public:
    Addition(exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right))
    {
        castToCommonType(_left, _right, true);
        bool valid = Sql::Operators::inferAddTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'+' requires numerical arguments");
        }
    }

    ~Addition() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

class Subtraction : public BinaryOperator {
public:
    Subtraction(exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right))
    {
        castToCommonType(_left, _right, true);
        bool valid = Sql::Operators::inferSubTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'-' requires numerical arguments");
        }
    }

    ~Subtraction() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

class Multiplication : public BinaryOperator {
public:
    Multiplication(exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right))
    {
        castToCommonType(_left, _right, true);
        bool valid = Sql::Operators::inferMulTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'*' requires numerical arguments");
        }
    }

    ~Multiplication() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

class Division : public BinaryOperator {
public:
    Division(exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right))
    {
        castToCommonType(_left, _right, true);
        bool valid = Sql::Operators::inferDivTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'/' requires numerical arguments");
        }
    }

    ~Division() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

using ComparisonMode = Sql::ComparisonMode;

class Comparison : public BinaryOperator {
public:
    ComparisonMode _mode;

    Comparison(ComparisonMode mode, exp_op_t left, exp_op_t right) :
            BinaryOperator(std::move(left), std::move(right)),
            _mode(mode)
    {
        castToCommonType(_left, _right);
        bool valid = Sql::Operators::inferCompareTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("cannot compare '" + Sql::getName(_left->getType()) +
                "' and '" + Sql::getName(_right->getType()) + "'.");
        }
    }

    ~Comparison() override { }

    /// a = b becomes b = a; only for an equality, whose sides may swap
    void swapSides()
    {
        assert(_mode == ComparisonMode::eq);
        std::swap(_left, _right);
    }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }

private:
    Sql::SqlType _type;
};

class Identifier : public Expression {
public:
    iu_p_t _iu;

    Identifier(iu_p_t iu) :
            _iu(iu)
    { }

    void accept(ExpressionVisitor & visitor) override;

    ~Identifier() override { }

    Sql::SqlType getType() const override
    {
        return _iu->sqlType;
    }
};

class Constant : public Expression {
public:
    std::string _value;
    Sql::SqlType _type;

    Constant(const std::string & value, Sql::SqlType type) :
            _value(value), _type(type)
    { }

    ~Constant() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }
};

class NullConstant : public Expression {
public:
    NullConstant() { }

    ~NullConstant() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return Sql::getUnknownTy();
    }
};

/// A function or an operator of arrays (see Native::Sql::Arrays::Function), which the runtime computes
class ArrayFunction : public Expression {
public:
    Native::Sql::Arrays::Function _function;
    Sql::SqlType _type;
    std::vector<exp_op_t> _arguments;

    ArrayFunction(Native::Sql::Arrays::Function function, Sql::SqlType type, std::vector<exp_op_t> arguments) :
            _function(function), _type(type), _arguments(std::move(arguments))
    { }

    ~ArrayFunction() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _type;
    }
};

class ExpressionVisitor {
public:
    void visit(Expression & exp)
    {
        unreachable();
    }

    virtual void visit(Cast & exp) = 0;
    virtual void visit(Not & exp) = 0;
    virtual void visit(IsNull & exp) = 0;
    virtual void visit(And & exp) = 0;
    virtual void visit(Or & exp) = 0;
    virtual void visit(Addition & exp) = 0;
    virtual void visit(Subtraction & exp) = 0;
    virtual void visit(Multiplication & exp) = 0;
    virtual void visit(Division & exp) = 0;
    virtual void visit(Comparison & exp) = 0;
    virtual void visit(Identifier & exp) = 0;
    virtual void visit(Constant & exp) = 0;
    virtual void visit(NullConstant & exp) = 0;
    virtual void visit(ArrayFunction & exp) = 0;
};

} // end namespace Expressions
} // end namespace Logical
} // end namespace Algebra
