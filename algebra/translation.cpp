#include "algebra/translation.hpp"

#include <memory>
#include <stack>

#include "algebra/physical/expressions.hpp"
#include "algebra/physical/operators.hpp"
#include "sql/SqlValues.hpp"

using namespace Algebra;

namespace Algebra {

using physical_aggregator_op_t = std::unique_ptr<Physical::Aggregations::Aggregator>;
using physical_expression_op_t = std::unique_ptr<Physical::Expressions::Expression>;
using physical_operator_op_t = std::unique_ptr<Physical::Operator>;

//-----------------------------------------------------------------------------
// Expression translation

class ExpressionTranslator : public Logical::Expressions::ExpressionVisitor {
public:
    ExpressionTranslator(Logical::Expressions::Expression & root)
    {
        root.accept(*this);
    }

    void traverse(Logical::Expressions::UnaryOperator & current)
    {
        current.getChild().accept(*this);
    }

    void traverse(Logical::Expressions::BinaryOperator & current)
    {
        current.getLeftChild().accept(*this);
        current.getRightChild().accept(*this);
    }

    physical_expression_op_t getResult()
    {
        assert(_translated.size() == 1);
        return std::move(_translated.top());
    }

    void visit(Logical::Expressions::Cast & exp)
    {
        // translate child
        traverse(exp);

        auto child = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Cast>( std::move(child), exp.getType() ) );
    }

    void visit(Logical::Expressions::Not & exp)
    {
        // translate child
        traverse(exp);

        auto child = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Not>( std::move(child) ) );
    }

    void visit(Logical::Expressions::And & exp)
    {
        // translate children
        traverse(exp);

        // the right child was visited last
        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::And>(
                exp.getType(),
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Or & exp)
    {
        // translate children
        traverse(exp);

        // the right child was visited last
        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Or>(
                exp.getType(),
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Addition & exp)
    {
        // translate children
        traverse(exp);

        // the right child was visited last
        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Addition>(
                exp.getType(),
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Subtraction & exp)
    {
        // translate children
        traverse(exp);

        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Subtraction>(
                exp.getType(),
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Multiplication & exp)
    {
        // translate children
        traverse(exp);

        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Multiplication>(
                exp.getType(),
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Division & exp)
    {
        // translate children
        traverse(exp);

        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();
        _translated.push( std::make_unique<Physical::Expressions::Division>(
                exp.getType(),
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Comparison & exp)
    {
        // translate children
        traverse(exp);

        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();

        _translated.push( std::make_unique<Physical::Expressions::Comparison>(
                exp.getType(),
                exp._mode,
                std::move(leftChild),
                std::move(rightChild)
        ) );
    }

    void visit(Logical::Expressions::Identifier & exp)
    {
        _translated.push( std::make_unique<Physical::Expressions::Identifier>(exp._iu) );
    }

    void visit(Logical::Expressions::Constant & exp)
    {
        Sql::value_op_t sqlValue = Sql::Value::castString(exp._value, exp.getType());
        _translated.push( std::make_unique<Physical::Expressions::Constant>( std::move(sqlValue) ) );
    }

    void visit(Logical::Expressions::NullConstant & exp)
    {
        _translated.push( std::make_unique<Physical::Expressions::NullConstant>() );
    }

private:
    std::stack<physical_expression_op_t> _translated;
};

//-----------------------------------------------------------------------------
// Aggregator translation

struct AggregatorTranslator : public Logical::Aggregations::AggregatorVisitor {
    std::unique_ptr<Physical::Aggregations::Aggregator> _result;
    Logical::Aggregations::Aggregator & _input;

    AggregatorTranslator(Logical::Aggregations::Aggregator & input) :
            _input(input)
    {
        input.accept(*this);
    }

    void visit(Logical::Aggregations::Keep & aggregator) override
    {
        const iu_set_t & required = _input.getRequired();
        if (required.size() != 1) {
            throw InvalidOperationException("'keep' should only require one iu");
        }
        _result = std::make_unique<Physical::Aggregations::Keep>(
                _input.getContext(), _input.getProduced(), *required.begin()
        );
    }

    void visit(Logical::Aggregations::Sum & aggregator) override
    {
        ExpressionTranslator expressionTranslator(aggregator.getExpression());
        physical_expression_op_t physicalExpression = expressionTranslator.getResult();
        _result = std::make_unique<Physical::Aggregations::Sum>(
                _input.getContext(), _input.getProduced(), std::move(physicalExpression)
        );
    }

    void visit(Logical::Aggregations::Avg & aggregator) override
    {
        ExpressionTranslator expressionTranslator(aggregator.getExpression());
        physical_expression_op_t physicalExpression = expressionTranslator.getResult();
        _result = std::make_unique<Physical::Aggregations::Avg>(
                _input.getContext(), _input.getProduced(), std::move(physicalExpression)
        );
    }

    void visit(Logical::Aggregations::CountAll & aggregator) override
    {
        _result = std::make_unique<Physical::Aggregations::CountAll>(_input.getContext(), _input.getProduced());
    }

    void visit(Logical::Aggregations::Min & aggregator) override
    {
        ExpressionTranslator expressionTranslator(aggregator.getExpression());
        physical_expression_op_t physicalExpression = expressionTranslator.getResult();
        _result = std::make_unique<Physical::Aggregations::Min>(
                _input.getContext(), _input.getProduced(), std::move(physicalExpression)
        );
    }
};

static physical_aggregator_op_t translateAggregator(Logical::Aggregations::Aggregator & aggregator)
{
    AggregatorTranslator translator(aggregator);
    return std::move(translator._result);
}

//-----------------------------------------------------------------------------
// Operator translation

class TreeTranslator : public Logical::OperatorVisitor {
public:
    std::stack<physical_operator_op_t> _translated;

    TreeTranslator(const Logical::Operator & root)
    {
        // I won't change anything. I promise.
        // The alternative would be a const version of OperatorVisitor.
        auto & current = const_cast<Logical::Operator &>(root);
        traverse(current);
    }

    // post order
    void traverse(Logical::Operator & current)
    {
        // ...
        switch (current.arity()) {
            case 0:
                // NOP
                break;
            case 1: {
                auto & unOp = dynamic_cast<const Logical::UnaryOperator &>(current);
                traverse(unOp.getChild());
                break;
            }
            case 2: {
                auto & binOp = dynamic_cast<const Logical::BinaryOperator &>(current);
                traverse(binOp.getLeftChild());
                traverse(binOp.getRightChild());
                break;
            }
            default:
                unreachable();
        }

        current.accept(*this); // translate
        // ...
    }

    physical_operator_op_t getResult()
    {
        assert(_translated.size() == 1);
        return std::move(_translated.top());
    }

    void visit(Logical::GroupBy & op) override
    {
        auto child = std::move(_translated.top());
        _translated.pop();

        std::vector<physical_aggregator_op_t> aggregations;
        for (auto & logicalAggregator : op._aggregations) {
            aggregations.push_back( translateAggregator(*logicalAggregator) );
        }

        _translated.push( std::make_unique<Physical::GroupBy>(
            op,
            std::move(child),
            std::move(aggregations)
        ) );
    }

    void visit(Logical::Join & op) override
    {
        auto rightChild = std::move( _translated.top() );
        _translated.pop();
        auto leftChild = std::move( _translated.top() );
        _translated.pop();

        bool equiConditionsOnly = true;
        std::vector<std::pair<physical_expression_op_t, physical_expression_op_t>> joinPairs;
        for (auto& joinExpr : op._joinExprVec) {
            if (Logical::Expressions::Comparison * cmp = dynamic_cast<Logical::Expressions::Comparison *>(joinExpr.get())) {
                if (cmp->_mode != Logical::Expressions::ComparisonMode::eq) {
                    throw NotImplementedException();
                    equiConditionsOnly = false;
                }

                ExpressionTranslator leftExprTranslator(cmp->getLeftChild());
                physical_expression_op_t leftExpr = leftExprTranslator.getResult();
                ExpressionTranslator rightExprTranslator(cmp->getRightChild());
                physical_expression_op_t rightExpr = rightExprTranslator.getResult();
                joinPairs.push_back(std::make_pair(std::move(leftExpr), std::move(rightExpr)));
            } else {
                throw NotImplementedException();
                // e.g. or construction within an expression -> block nested loop join
            }
        }

        switch (op._method) {
            case Logical::Join::Method::Hash: {
                _translated.push( std::make_unique<Physical::HashJoin>(
                    op,
                    std::move(leftChild),
                    std::move(rightChild),
                    std::move(joinPairs)
                ) );
                break;
            }
            default:
                throw NotImplementedException();
        }
    }

    void visit(Logical::Map & op) override
    {
        throw NotImplementedException();
    }

    void visit(Logical::Insert & op) override
    {
        _translated.push( std::make_unique<Physical::Insert>(
                op,
                op.getTable(),
                op.getTuple(),
                op.getContext()
        ) );
    }

    void visit(Logical::Update & op) override
    {
        auto child = std::move(_translated.top());
        _translated.pop();

        _translated.push( std::make_unique<Physical::Update>(
                op,
                std::move(child),
                op.getTable(),
                op.getUpdateIUValuePairs()
        ) );
    }

    void visit(Logical::Delete & op) override
    {
        auto child = std::move(_translated.top());
        _translated.pop();

        _translated.push( std::make_unique<Physical::Delete>(
                op,
                std::move(child),
                op.getTable()
        ));
    }

    void visit(Logical::Result & op) override
    {
        auto child = std::move(_translated.top());
        _translated.pop();

        switch (op._type) {
            case Logical::Result::Type::PrintToStdOut: {
                _translated.push( std::make_unique<Physical::Print>(
                        op,
                        std::move(child)
                ) );
                break;
            }
            default:
                throw NotImplementedException();
        }
    }

    void visit(Logical::Select & op) override
    {
        auto child = std::move(_translated.top());
        _translated.pop();

        ExpressionTranslator expTranslator(*op._exp);
        physical_expression_op_t exp = expTranslator.getResult();

        _translated.push( std::make_unique<Physical::Select>(
            op,
            std::move(child),
            std::move(exp)
        ) );
    }

    void visit(Logical::TableScan & op) override
    {
        _translated.push( std::make_unique<Physical::TableScan>(
            op,
            op.getTable(),
            op.getAlias()
        ) );
    }

};

std::unique_ptr<Physical::Operator> translateToPhysicalTree(const Logical::Operator & resultOperator)
{
    TreeTranslator t(resultOperator);
    return t.getResult();
}

} // end namespace Algebra
