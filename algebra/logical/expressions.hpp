
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
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlOperators.hpp"

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
};

using exp_op_t = std::unique_ptr<Expression>;
using logical_exp_op_t = exp_op_t;

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

    Cast(exp_op_t child, Sql::SqlType destType) :
            UnaryOperator(std::move(child)),
            _destType(destType)
    { }

    ~Cast() override { }

    void accept(ExpressionVisitor & visitor) override;

    Sql::SqlType getType() const override
    {
        return _destType;
    }
};

class Not : public UnaryOperator {
public:
    Not(exp_op_t child) :
            UnaryOperator(std::move(child))
    {
        assert(child->getType().typeID == Sql::SqlType::TypeID::BoolID);
    }

    ~Not() override { }

    void accept(ExpressionVisitor & visitor) override;
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

        // TODO null values?
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

        // TODO null values?
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
        bool valid = Sql::Operators::inferAddTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'+' requires numerical arguments");
        }

        // cast if necessary
        if (!Sql::sameTypeID(_type, _left->getType())) {
            auto newLeft = std::make_unique<Cast>(std::move(_left), _right->getType());
            _left = std::move(newLeft);

            valid = Sql::Operators::inferAddTy(_left->getType(), _right->getType(), _type);
            assert(valid);
        } else if (!Sql::sameTypeID(_type, _right->getType())) {
            auto newRight = std::make_unique<Cast>(std::move(_right), _left->getType());
            _right = std::move(newRight);

            valid = Sql::Operators::inferAddTy(_left->getType(), _right->getType(), _type);
            assert(valid);
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
        bool valid = Sql::Operators::inferSubTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'-' requires numerical arguments");
        }

        // cast if necessary
        if (!Sql::sameTypeID(_type, _left->getType())) {
            auto newLeft = std::make_unique<Cast>(std::move(_left), _right->getType());
            _left = std::move(newLeft);

            valid = Sql::Operators::inferSubTy(_left->getType(), _right->getType(), _type);
            assert(valid);
        } else if (!Sql::sameTypeID(_type, _right->getType())) {
            auto newRight = std::make_unique<Cast>(std::move(_right), _left->getType());
            _right = std::move(newRight);

            valid = Sql::Operators::inferSubTy(_left->getType(), _right->getType(), _type);
            assert(valid);
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
        bool valid = Sql::Operators::inferMulTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'*' requires numerical arguments");
        }

        // cast if necessary
        if (!Sql::sameTypeID(_type, _left->getType())) {
            auto newLeft = std::make_unique<Cast>(std::move(_left), _right->getType());
            _left = std::move(newLeft);

            valid = Sql::Operators::inferMulTy(_left->getType(), _right->getType(), _type);
            assert(valid);
        } else if (!Sql::sameTypeID(_type, _right->getType())) {
            auto newRight = std::make_unique<Cast>(std::move(_right), _left->getType());
            _right = std::move(newRight);

            valid = Sql::Operators::inferMulTy(_left->getType(), _right->getType(), _type);
            assert(valid);
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
        bool valid = Sql::Operators::inferDivTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("'/' requires numerical arguments");
        }

        // cast if necessary
        if (!Sql::sameTypeID(_type, _left->getType())) {
            auto newLeft = std::make_unique<Cast>(std::move(_left), _right->getType());
            _left = std::move(newLeft);

            valid = Sql::Operators::inferDivTy(_left->getType(), _right->getType(), _type);
            assert(valid);
        } else if (!Sql::sameTypeID(_type, _right->getType())) {
            auto newRight = std::make_unique<Cast>(std::move(_right), _left->getType());
            _right = std::move(newRight);

            valid = Sql::Operators::inferDivTy(_left->getType(), _right->getType(), _type);
            assert(valid);
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
        bool valid = Sql::Operators::inferCompareTy(_left->getType(), _right->getType(), _type);
        if (!valid) {
            throw ArithmeticException("cannot compare '" + Sql::getName(_left->getType()) +
                "' and '" + Sql::getName(_right->getType()) + "'.");
        }
    }

    ~Comparison() override { }

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

class ExpressionVisitor {
public:
    void visit(Expression & exp)
    {
        unreachable();
    }

    virtual void visit(Cast & exp) = 0;
    virtual void visit(Addition & exp) = 0;
    virtual void visit(Subtraction & exp) = 0;
    virtual void visit(Multiplication & exp) = 0;
    virtual void visit(Division & exp) = 0;
    virtual void visit(Comparison & exp) = 0;
    virtual void visit(Identifier & exp) = 0;
    virtual void visit(Constant & exp) = 0;
    virtual void visit(NullConstant & exp) = 0;
};

} // end namespace Expressions
} // end namespace Logical
} // end namespace Algebra
