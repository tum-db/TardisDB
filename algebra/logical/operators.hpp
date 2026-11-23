
#pragma once

#include <cstdint>
#include <memory>
#include "native/sql/SqlTuple.hpp"

#include "algebra/logical/expressions.hpp"
#include "foundations/InformationUnit.hpp"
#include "foundations/IUFactory.hpp"

namespace Algebra {
namespace Logical {

using Logical::Expressions::logical_exp_op_t;

struct OperatorVisitor;

//-----------------------------------------------------------------------------
// Operator

class Operator {
public:
    size_t cardinality;

    Operator(IUFactory &iuFactory) :
            _iuFactory(iuFactory)
    {
        _uid = _iuFactory.getUID();
    }

    Operator(const Operator &) = delete;

    Operator & operator=(const Operator &) = delete;

    virtual ~Operator() { };

    virtual void accept(OperatorVisitor & visitor) = 0;

    IUFactory & getIUFactory() const { return _iuFactory; }

    Operator * getParent() const { return parent; }

    Operator * getRoot();

    virtual size_t arity() const = 0;

    const iu_set_t & getProduced() const;

    const iu_set_t & getRequired() const;

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
    IUFactory &_iuFactory;

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
class Update;
class Insert;
class Delete;
class Result;
class Select;
class TableScan;
class BranchAggregation;
class BranchDiff;

struct OperatorVisitor {
    void visit(Operator & op)
    {
        unreachable();
    }

    virtual void visit(GroupBy & op) = 0;
    virtual void visit(Join & op) = 0;
    virtual void visit(Map & op) = 0;
    virtual void visit(Update & op) = 0;
    virtual void visit(Insert & op) = 0;
    virtual void visit(Delete & op) = 0;
    virtual void visit(Result & op) = 0;
    virtual void visit(Select & op) = 0;
    virtual void visit(TableScan & op) = 0;
    virtual void visit(BranchAggregation & op) = 0;
    virtual void visit(BranchDiff & op) = 0;
};

//-----------------------------------------------------------------------------
// NullaryOperator

class NullaryOperator : public Operator {
public:
    NullaryOperator(IUFactory &iuFactory);

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

    const iu_set_t & getLeftRequired() const { return leftRequired; }
    const iu_set_t & getRightRequired() const { return rightRequired; }

protected:
    void updateProducedSetsTraverser() final;
    void updateRequiredSetsTraverser() final;

    void splitRequiredSet();

    std::unique_ptr<Operator> leftChild;
    std::unique_ptr<Operator> rightChild;

    iu_set_t leftRequired;
    iu_set_t rightRequired;
};

//-----------------------------------------------------------------------------
// Map operator

/// the attribute (an IU of a value) that the expression computes
struct Mapping {
    iu_p_t src;
    logical_exp_op_t exp;
};

/// Computes attributes of the tuples of its input (the computed columns of a select list)
class Map : public UnaryOperator {
public:
    Map(std::unique_ptr<Operator> input, std::vector<Mapping> mappings) :
            UnaryOperator(std::move(input)),
            _mappings(std::move(mappings))
    { }

    std::vector<Mapping> _mappings;

    void accept(OperatorVisitor & visitor) override;

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

using join_expr_vec_t = std::vector<logical_exp_op_t>;

class Join : public BinaryOperator {
public:
    enum class Method { Hash } _method;

    join_expr_vec_t _joinExprVec;

    Join(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, join_expr_vec_t joinExprVec, Method method) :
            BinaryOperator(std::move(left), std::move(right)),
            _method(method),
            _joinExprVec(std::move(joinExprVec))
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
    Aggregator(IUFactory &iuFactory) :
            _iuFactory(iuFactory)
    { }

    virtual ~Aggregator() { }

    virtual void accept(AggregatorVisitor & visitor) = 0;

    IUFactory & getIUFactory() const { return _iuFactory; }
    iu_p_t getProduced();

    // the expression could contain multiple iu references
    const iu_set_t & getRequired();

    virtual Sql::SqlType getResultType() const = 0;

    friend class Algebra::Logical::GroupBy;

protected:
    virtual void computeProduced();
    virtual void computeRequired() = 0;

    IUFactory & _iuFactory;
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
    Keep(IUFactory &iuFactory, iu_p_t keep) :
            Aggregator(iuFactory),
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
    Sum(IUFactory &iuFactory, logical_exp_op_t exp) :
            Aggregator(iuFactory),
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
    Avg(IUFactory &iuFactory, logical_exp_op_t exp);

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
    CountAll(IUFactory &iuFactory) :
            Aggregator(iuFactory)
    { }

    ~CountAll() override { }

    void accept(AggregatorVisitor & visitor) override { visitor.visit(*this); }

    Sql::SqlType getResultType() const override { return Sql::getIntegerTy(); }

protected:
    void computeRequired() override;
};

class Min : public Aggregator {
public:
    Min(IUFactory &iuFactory, logical_exp_op_t exp) :
            Aggregator(iuFactory),
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
// Update operator

class Delete : public UnaryOperator {
public:
    Delete(std::unique_ptr<Operator> child, iu_p_t &tidIU, Table & table, branch_id_t branchId);

    ~Delete() override;

    void accept(OperatorVisitor & visitor) override;

    Table & getTable() const { return _table; }

    iu_p_t &getTIDIU() { return tidIU; }

    branch_id_t getBranchId() { return branchId; }
protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;

    iu_p_t tidIU;
    branch_id_t branchId;
};

//-----------------------------------------------------------------------------
// Insert operator

class Insert : public NullaryOperator {
public:
    Insert(IUFactory &iuFactory, Table & table, std::vector<Native::Sql::SqlTuple *> tuples, branch_id_t branchId);

    ~Insert() override;

    void accept(OperatorVisitor & visitor) override;

    Table & getTable() const { return _table; }

    std::vector<Native::Sql::SqlTuple *> &getTuples() { return sqlTuples; }

    branch_id_t getBranchId() { return branchId; }
protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;
    branch_id_t branchId;
    std::vector<Native::Sql::SqlTuple *> sqlTuples;
};

//-----------------------------------------------------------------------------
// Update operator

class Update : public UnaryOperator {
public:
    Update(std::unique_ptr<Operator> child, std::vector<std::pair<iu_p_t,std::string>> &updateIUValuePairs, Table & table, branch_id_t branchId);


    ~Update() override;

    void accept(OperatorVisitor & visitor) override;

    Table & getTable() const { return _table; }

    branch_id_t getBranchId() { return branchId; }

    std::vector<std::pair<iu_p_t,std::string>> &getUpdateIUValuePairs() { return updateIUValuePairs; }

    /// SET c = <expression>: the new value of the column of the iu is computed from the old tuple
    void addUpdateExpression(iu_p_t iu, logical_exp_op_t expression) { updateExpressions.emplace_back(iu, std::move(expression)); }

    std::vector<std::pair<iu_p_t, logical_exp_op_t>> updateExpressions;

protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;
    branch_id_t branchId;

    std::vector<std::pair<iu_p_t,std::string>> updateIUValuePairs;
};

//-----------------------------------------------------------------------------
// Result operator

class Result : public UnaryOperator {
public:
    static enum class Type { PrintToStdOut, TupleStreamHandler } _type;

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
    TableScan(IUFactory &iuFactory, Table & table, branch_id_t branchId) :
            NullaryOperator(iuFactory),
            _table(table),
            branchId(branchId)
    { }

    TableScan(IUFactory &iuFactory, Table & table) :
            NullaryOperator(iuFactory),
            _table(table)
    { }

    ~TableScan() override { }

    void accept(OperatorVisitor & visitor) override;

    Table & getTable() const { return _table; }

    branch_id_t getBranchId() { return branchId; }

    /// Scans only the candidates of the index for the key with the given hash
    void setIndexProbe(const BranchIndex * index, uint64_t keyHash) { _index = index; _indexKeyHash = keyHash; }

    const BranchIndex * getIndex() const { return _index; }

    uint64_t getIndexKeyHash() const { return _indexKeyHash; }

protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;
    branch_id_t branchId;
    const BranchIndex * _index = nullptr;
    uint64_t _indexKeyHash = 0;
};

//-----------------------------------------------------------------------------
// BranchAggregation operator

/// Scans one table in a set of branches and aggregates per branch:
/// SELECT branch, sum(x) FROM t VERSION ( b1 , b2 ) GROUP BY branch
class BranchAggregation : public NullaryOperator {
public:
    enum class Function { Count, Sum, Min, Max, Avg };

    struct Aggregate {
        Function function;
        iu_p_t input; // nullptr for count(*)
        iu_p_t produced;
    };

    /// Shared: one pass over the versions shared by the branches; PerBranch: one scan per branch
    enum class Strategy { Shared, PerBranch };
    static Strategy _strategy;

    BranchAggregation(IUFactory &iuFactory, Table & table, std::vector<branch_id_t> branches);

    ~BranchAggregation() override { }

    void accept(OperatorVisitor & visitor) override;

    /// \returns The iu of a column of the scanned table, to be used by predicates and aggregates
    iu_p_t getColumnIU(const std::string & columnName);

    /// \returns The iu holding the name of the branch
    iu_p_t addBranchOutput();

    /// \returns The iu holding the result of the aggregate
    iu_p_t addAggregate(Function function, iu_p_t input);

    void addPredicate(logical_exp_op_t predicate);

    Table & getTable() const { return _table; }

    const std::vector<branch_id_t> & getBranches() const { return _branches; }

    iu_p_t getBranchIU() const { return _branchIU; }

    const std::vector<Aggregate> & getAggregates() const { return _aggregates; }

    std::vector<logical_exp_op_t> _predicates;

protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;
    std::vector<branch_id_t> _branches;
    std::unordered_map<std::string, iu_p_t> _columnIUs;
    iu_p_t _branchIU = nullptr;
    std::vector<Aggregate> _aggregates;
};

//-----------------------------------------------------------------------------
// BranchDiff operator

/// The tuples of one table that differ between two branches, each with the branch it is seen in:
/// SELECT * FROM DIFF ( t VERSION a , t VERSION b )
class BranchDiff : public NullaryOperator {
public:
    BranchDiff(IUFactory &iuFactory, Table & table, branch_id_t first, branch_id_t second);

    ~BranchDiff() override { }

    void accept(OperatorVisitor & visitor) override;

    /// \returns The iu of a column of the table
    iu_p_t getColumnIU(const std::string & columnName);

    /// \returns The iu holding the name of the branch
    iu_p_t getBranchIU();

    void addPredicate(logical_exp_op_t predicate) { _predicates.push_back(std::move(predicate)); }

    Table & getTable() const { return _table; }

    branch_id_t getFirst() const { return _first; }

    branch_id_t getSecond() const { return _second; }

    std::vector<logical_exp_op_t> _predicates;

protected:
    void computeProduced() override;
    void computeRequired() override;

    Table & _table;
    branch_id_t _first;
    branch_id_t _second;
    std::unordered_map<std::string, iu_p_t> _columnIUs;
    iu_p_t _branchIU = nullptr;
};

//-----------------------------------------------------------------------------
// Utils

/// \returns The attributes the parent operator expects from the child
iu_set_t computeExpected(Operator * parent, Operator * child);

iu_set_t collectRequired(const Expressions::Expression & exp);

bool verifyDependencies(Result & root);

} // end namespace Logical
} // end namespace Algebra
