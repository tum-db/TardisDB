
#pragma once

#include <set>
#include <vector>
#include <memory>

#include "codegen/CodeGen.hpp"
#include "foundations/InformationUnit.hpp"
#include "foundations/QueryContext.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

using iu_value_mapping_t = std::unordered_map<iu_p_t, Sql::Value *>;

class Operator {
public:
    Operator(QueryContext & context, const iu_set_t & required);

    virtual ~Operator();

    Operator(const Operator &) = delete;

    Operator & operator=(const Operator &) = delete;

    QueryContext & getContext() const;

    Operator * getParent() const;

    virtual void produce() = 0;

    /// \note It is guaranteed that "values" contains at least all required ius.
    /// However, it can also contain additional ius, these ius should be ignored.
    virtual void consume(const iu_value_mapping_t & values, const Operator & src) = 0;

    virtual size_t arity() const = 0;

    const iu_set_t & getRequired();

    friend class NullaryOperator;
    friend class UnaryOperator;
    friend class BinaryOperator;

protected:
    void setParent(Operator * parent);

    Operator * parent = nullptr;
    CodeGen & codeGen;
    QueryContext & context;

    iu_set_t _required;
};

class NullaryOperator : public Operator {
public:
    NullaryOperator(QueryContext & context, const iu_set_t & required);

    virtual ~NullaryOperator();

    void consume(const iu_value_mapping_t & values, const Operator & src) final;

    size_t arity() const final;
};

class UnaryOperator : public Operator {
public:
    UnaryOperator(std::unique_ptr<Operator> input, const iu_set_t & required);

    virtual ~UnaryOperator();

    size_t arity() const final;

protected:
    std::unique_ptr<Operator> child;
};

class BinaryOperator : public Operator {
public:
    BinaryOperator(std::unique_ptr<Operator> leftInput, std::unique_ptr<Operator> rightInput,
                   const iu_set_t & required);

    virtual ~BinaryOperator();

    void consume(const iu_value_mapping_t & values, const Operator & src) final;

    size_t arity() const final;

protected:
    virtual void consumeLeft(const iu_value_mapping_t & values) = 0;
    virtual void consumeRight(const iu_value_mapping_t & values) = 0;

    std::unique_ptr<Operator> leftChild;
    std::unique_ptr<Operator> rightChild;
};

} // end namespace Physical
} // end namespace Algebra
