
#pragma once

#include <cstdint>
#include <memory>

#include "algebra/logical/expressions.hpp"
#include "foundations/InformationUnit.hpp"
#include "foundations/QueryContext.hpp"

namespace Algebra {
namespace Logical {

using Logical::Expressions::logical_exp_op_t;

struct OperatorVisitor;

//-----------------------------------------------------------------------------
// Operator

class Operator {
public:
    size_t cardinality;

    Operator(QueryContext & context) :
            _context(context)
    {
        _uid = _context.operatorUID;
        _context.operatorUID += 1;
    }

    Operator(const Operator &) = delete;

    Operator & operator=(const Operator &) = delete;

    virtual ~Operator() { };

    virtual void accept(OperatorVisitor & visitor) = 0;

    QueryContext & getContext() const { return _context; }

    Operator * getParent() const { return parent; }

    Operator * getRoot();

    virtual size_t arity() const = 0;

    const iu_set_t & getProduced();

    const iu_set_t & getRequired();

    uint32_t getUID() const { return _uid; }

    // TODO
//    virtual void swap(Operator & other) = 0;

    friend class NullaryOperator;
    friend class UnaryOperator;
    friend class BinaryOperator;

protected:
    virtual void computeProduced() = 0;
    virtual void computeRequired() = 0;

    void updateProducedSets();
    virtual void updateProducedSetsTraverser() = 0;

    void updateRequiredSets();
    virtual void updateRequiredSetsTraverser() = 0;

    Operator * parent = nullptr;
    QueryContext & _context;

    iu_set_t produced;
    iu_set_t required;
    bool producedUpToDate = false;
    bool requiredUpToDate = false;

private:
    uint32_t _uid;
};

//-----------------------------------------------------------------------------
// OperatorVisitor

class GroupBy;
class Join;
class Map;
class Result;
class Select;
class TableScan;

struct OperatorVisitor {
    void visit(Operator & op)
    {
        unreachable();
    }

    virtual void visit(GroupBy & op) = 0;
    virtual void visit(Join & op) = 0;
    virtual void visit(Map & op) = 0;
    virtual void visit(Result & op) = 0;
    virtual void visit(Select & op) = 0;
    virtual void visit(TableScan & op) = 0;
};

//-----------------------------------------------------------------------------
// NullaryOperator

class NullaryOperator : public Operator {
public:
    NullaryOperator(QueryContext & context);

    ~NullaryOperator() override;

    size_t arity() const final;

protected:
    void updateProducedSetsTraverser() final;
    void updateRequiredSetsTraverser() final;
};

//-----------------------------------------------------------------------------
// UnaryOperator

class UnaryOperator : public Operator {
public:
    UnaryOperator(std::unique_ptr<Operator> input);

    ~UnaryOperator() override;

    size_t arity() const final;

    Operator & getChild() const { return *child; }

    // see swap()
//    std::unique_ptr<Operator> takeChild();

protected:
    void updateProducedSetsTraverser() final;
    void updateRequiredSetsTraverser() final;

    std::unique_ptr<Operator> child;
};

//-----------------------------------------------------------------------------
// BinaryOperator

class BinaryOperator : public Operator {
public:
    BinaryOperator(std::unique_ptr<Operator> leftInput, std::unique_ptr<Operator> rightInput);

    ~BinaryOperator() override;

    size_t arity() const final;

    Operator & getLeftChild() const { return *leftChild; }
    Operator & getRightChild() const { return *rightChild; }

    // see swap()
//    std::unique_ptr<Operator> takeLeftChild();
//    std::unique_ptr<Operator> takeRightChild();

protected:
    void updateProducedSetsTraverser() final;
    void updateRequiredSetsTraverser() final;

    std::unique_ptr<Operator> leftChild;
    std::unique_ptr<Operator> rightChild;
};

//-----------------------------------------------------------------------------
// Map operator

struct Mapping {
    iu_p_t src;
    logical_exp_op_t exp;
};

class Map : public UnaryOperator {
public:
    // single mapping: iu -> exp -> new iu
    Map(std::unique_ptr<Operator> input, std::vector<Mapping> mappings) :
            UnaryOperator(std::move(input)),
            _mappings(std::move(mappings))
    { }

    std::vector<Mapping> _mappings;

protected:
    void computeProduced() override;
    void computeRequired() override;
};

//-----------------------------------------------------------------------------
// Select operator

class Select : public UnaryOperator {
public:
    logical_exp_op_t _exp;

    Select(std::unique_ptr<Operator> input, logical_exp_op_t exp) :
            UnaryOperator(std::move(input)),
            _exp(std::move(exp))
    { }

    ~Select() override { };

    void accept(OperatorVisitor & visitor) override;

protected:
    void computeProduced() override;
    void computeRequired() override;
};

//-----------------------------------------------------------------------------
// Join operator

class Join : public BinaryOperator {
public:
    enum class Method { Hash } _method;

    logical_exp_op_t _joinExp;

    Join(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, logical_exp_op_t exp, Method method) :
            BinaryOperator(std::move(left), std::move(right)),
            _method(method),
            _joinExp(std::move(exp))
    { }

    ~Join() override { };

    void accept(OperatorVisitor & visitor) override;

protected:
    void computeProduced() override;
    void computeRequired() override;
};

//-----------------------------------------------------------------------------
// GroupBy operator

namespace Aggregations {

struct AggregatorVisitor;

class Aggregator {
public:
    Aggregator(QueryContext & context) :
            _context(context)
    { }

    virtual ~Aggregator() { }

    virtual void accept(AggregatorVisitor & visitor) = 0;

    QueryContext & getContext() const { return _context; }

    iu_p_t getProduced();

    // the expression could contain multiple iu references
    const iu_set_t & getRequired();

    virtual Sql::SqlType getResultType() const = 0;

    friend class Algebra::Logical::GroupBy;

protected:
    virtual void computeProduced();
    virtual void computeRequired() = 0;

    QueryContext & _context;
    Operator * parent = nullptr;

    iu_p_t _produced;
    iu_set_t _required;
    bool _producedUpToDate = false;
    bool _requiredUpToDate = false;
};

class Keep;
class Sum;
class Avg;
class CountAll;
class Min;

struct AggregatorVisitor {
    void visit(Aggregator & aggregator)
    {
        unreachable();
    }

    virtual void visit(Keep & aggregator) = 0;
    virtual void visit(Sum & aggregator) = 0;
    virtual void visit(Avg & aggregator) = 0;
    virtual void visit(CountAll & aggregator) = 0;
    virtual void visit(Min & aggregator) = 0;
};

class Keep : public Aggregator {
public:
    Keep(QueryContext & context, iu_p_t keep) :
            Aggregator(context),
            _keep(keep)
    { }

    ~Keep() override { }

    void accept(AggregatorVisitor & visitor) override { visitor.visit(*this); }

    Sql::SqlType getResultType() const override { return _keep->sqlType; }

protected:
    void computeRequired() override;

    iu_p_t _keep;
};

class Sum : public Aggregator {
public:
    Sum(QueryContext & context, logical_exp_op_t exp) :
            Aggregator(context),
            _expression(std::move(exp))
    { }

    ~Sum() override { }

    void accept(AggregatorVisitor & visitor) override { visitor.visit(*this); }

    Sql::SqlType getResultType() const override { return _expression->getType(); }

    Expressions::Expression & getExpression() const { return *_expression; }

private:
    void computeRequired() override;

    logical_exp_op_t _expression;
};

class Avg : public Aggregator {
public:
    Avg(QueryContext & context, logical_exp_op_t exp);

    ~Avg() override { }

    void accept(AggregatorVisitor & visitor) override { visitor.visit(*this); }

    Sql::SqlType getResultType() const override { return _expression->getType(); }

    Expressions::Expression & getExpression() const { return *_expression; }

private:
    void computeRequired() override;

    logical_exp_op_t _expression;
};

class CountAll : public Aggregator {
public:
    CountAll(QueryContext & context) :
            Aggregator(context)
    { }

    ~CountAll() override { }

    void accept(AggregatorVisitor & visitor) override { visitor.visit(*this); }

    Sql::SqlType getResultType() const override { return Sql::getIntegerTy(); }

protected:
    void computeRequired() override;
};

class Min : public Aggregator {
public:
    Min(QueryContext & context, logical_exp_op_t exp) :
            Aggregator(context),
            _expression(std::move(exp))
    { }

    ~Min() override { }

    void accept(AggregatorVisitor & visitor) override { visitor.visit(*this); }

    Sql::SqlType getResultType() const override { return _expression->getType(); }

    Expressions::Expression & getExpression() const { return *_expression; }

private:
    void computeRequired() override;

    logical_exp_op_t _expression;
};

} // end namespace Aggregations

class GroupBy : public UnaryOperator {
public:
    std::vector<std::unique_ptr<Aggregations::Aggregator>> _aggregations;

    GroupBy(std::unique_ptr<Operator> input, std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations);

    ~GroupBy() override { }

    void accept(OperatorVisitor & visitor) override;

protected:
    void computeProduced() override;
    void computeRequired() override;

};

//-----------------------------------------------------------------------------
// Result operator

class Result : public UnaryOperator {
public:
    enum class Type { PrintToStdOut } _type = Type::PrintToStdOut;

    std::vector<iu_p_t> selection;

    Result(std::unique_ptr<Operator> child, const std::vector<iu_p_t> & selection);

    ~Result() override;

    void accept(OperatorVisitor & visitor) override;

protected:
    void computeProduced() override;
    void computeRequired() override;
};

//-----------------------------------------------------------------------------
// TableScan operator

class TableScan : public NullaryOperator {
public:
    TableScan(QueryContext & context, Table & table) :
            NullaryOperator(context),
            _table(table)
    { }

    ~TableScan() override { }

    void accept(OperatorVisitor & visitor) override;

    Table & getTable() const { return _table; }

protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;
};

//-----------------------------------------------------------------------------
// Utils

/// \returns The attributes the parent operator expects from the child
iu_set_t computeExpected(Operator * parent, Operator * child);

iu_set_t collectRequired(const Expressions::Expression & exp);

bool verifyDependencies(Result & root);

} // end namespace Logical
} // end namespace Algebra
