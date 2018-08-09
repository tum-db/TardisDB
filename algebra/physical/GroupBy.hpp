
#pragma once

#include "Operator.hpp"

#include "algebra/physical/expressions.hpp"

namespace Algebra {
namespace Physical {

namespace Aggregations {

class Aggregator {
public:
    Aggregator(QueryContext & context, iu_p_t produced);

    virtual ~Aggregator();

    iu_p_t getProduced();

    virtual void consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr) = 0;

    virtual void consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr) = 0;

    virtual Sql::SqlType getResultType() = 0;

    virtual Sql::value_op_t getResult(llvm::Value * entryPtr) = 0;

    virtual llvm::Type * getEntryType() = 0;

protected:
    QueryContext & _context;
    iu_p_t _produced;
};

class Keep : public Aggregator {
public:
    Keep(QueryContext & context, iu_p_t produced, iu_p_t keep);

    ~Keep() override;

    void consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    void consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    Sql::SqlType getResultType() override;

    Sql::value_op_t getResult(llvm::Value * entryPtr) override;

    llvm::Type * getEntryType() override;

    iu_p_t getKeepIU() const { return _keep; }

protected:
    iu_p_t _keep;
};

class Sum : public Aggregator {
public:
    Sum(QueryContext & context, iu_p_t produced, Expressions::exp_op_t exp);

    ~Sum() override;

    void consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    void consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    Sql::SqlType getResultType() override;

    Sql::value_op_t getResult(llvm::Value * entryPtr) override;

    llvm::Type * getEntryType() override;

private:
    Expressions::exp_op_t _expression;
};

class Avg : public Aggregator {
public:
    Avg(QueryContext & context, iu_p_t produced, Expressions::exp_op_t exp);

    ~Avg() override;

    void consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    void consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    Sql::SqlType getResultType() override;

    Sql::value_op_t getResult(llvm::Value * entryPtr) override;

    llvm::Type * getEntryType() override;

private:
    void createEntryType();

    Expressions::exp_op_t _expression;

    llvm::Type * _entryTy;
};

class CountAll : public Aggregator {
public:
    CountAll(QueryContext & context, iu_p_t produced);

    ~CountAll() override;

    void consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    void consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    Sql::SqlType getResultType() override;

    Sql::value_op_t getResult(llvm::Value * entryPtr) override;

    llvm::Type * getEntryType() override;
};

class Min : public Aggregator {
public:
    Min(QueryContext & context, iu_p_t produced, Expressions::exp_op_t exp);

    ~Min() override;

    void consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    void consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr) override;

    Sql::SqlType getResultType() override;

    Sql::value_op_t getResult(llvm::Value * entryPtr) override;

    llvm::Type * getEntryType() override;

private:
    Expressions::exp_op_t _expression;
};

#if 0
class Count : public Aggregator {
    Count();
    Count(ciu_t iu);
};

class CountDistinct : public Aggregator {
    std::set<int> seen;
};
#endif

} // end namespace Aggregations

/// The "group by" operator
class GroupBy : public UnaryOperator {
public:
    GroupBy(std::unique_ptr<Operator> input, const iu_set_t & required,
            std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations);

    virtual ~GroupBy();

    void produce() override;
    void consume(const iu_value_mapping_t & values, const Operator & src) override;

private:
    void aggregateInto(cg_voidptr_t group, const iu_value_mapping_t & values);

    void initializeGroup(cg_voidptr_t group, const iu_value_mapping_t & values);

    cg_hash_t currentHash(const iu_value_mapping_t & values);

    void createGroupType(); // struct of all aggregation entries

    std::pair<std::vector<Sql::value_op_t>, iu_value_mapping_t> getResult(cg_voidptr_t group);

    std::vector<std::unique_ptr<Aggregations::Aggregator>> _aggregations;

    iu_set_t _keepSet;
    bool _keepMode = false;

    cg_voidptr_t _hashTable;
    cg_voidptr_t _singleGroup;

    size_t _groupSize;

    llvm::Type * _groupType;

    llvm::AllocaInst * _emptyStackVar;
};

} // end namespace Physical
} // end namespace Algebra
