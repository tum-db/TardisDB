
#pragma once

#include <string>

#include "CodeGen.hpp"
#include "exceptions.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "utils.hpp"

template<class T>
class PhiNode;

template<>
class PhiNode<llvm::Value> {
public:
    CodeGen & codeGen;

    PhiNode(llvm::Value * initialValue, const std::string & name) :
            codeGen(getThreadLocalCodeGen()),
            _name(name)
    {
        _incoming.emplace_back(initialValue, codeGen->GetInsertBlock());
    }

    PhiNode(const PhiNode & other) = delete;

    PhiNode(PhiNode && other) :
        codeGen(other.codeGen),
        _created (other._created),
        _name(std::move(other._name)),
        _node(other._node),
        _incoming(std::move(other._incoming))
    {
        other._created = true;
        other._node = nullptr;
        other._incoming.clear();
    }

    ~PhiNode()
    {
        assert(_created);
    }

    void addIncoming(llvm::Value * value)
    {
        assert(value->getType() == _incoming.front().first->getType());
        _incoming.emplace_back(value, codeGen->GetInsertBlock());

        if (_created) {
            llvm::BasicBlock * currentBB = codeGen->GetInsertBlock();
            _node->addIncoming(value, currentBB);
        }
    }

    PhiNode & operator =(llvm::Value * value)
    {
        addIncoming(value);
        return *this;
    }

    llvm::Value * create()
    {
        if (_created) {
            return _node;
        }
        _created = true;

        auto * firstValue = _incoming.front().first;
        llvm::BasicBlock * currentBB = codeGen->GetInsertBlock();
        unsigned size = static_cast<unsigned>(_incoming.size());

        auto & instList = currentBB->getInstList();
        if (instList.empty()) {
            _node = llvm::PHINode::Create(firstValue->getType(), size, _name, currentBB);
        } else {
            auto * first = &currentBB->getInstList().front();
            _node = llvm::PHINode::Create(firstValue->getType(), size, _name, first);
        }

        for (auto & inPair : _incoming) {
            _node->addIncoming(inPair.first, inPair.second);
        }

        return _node;
    }

    llvm::Value * get()
    {
        if (_created) {
            return _node;
        } else {
            return create();
        }
    }

    operator llvm::Value *()
    {
        return get();
    }

private:
    bool _created = false;
    std::string _name;
    llvm::PHINode * _node;
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> _incoming;
};

/// a phi node for sql values (it is based on the decompostion and composition of the actual llvm values)
// TODO update to match the above version's interface
template<>
class PhiNode<Sql::Value> {
public:
    CodeGen & codeGen;

    PhiNode(const Sql::Value & initialValue, const std::string & name) :
            codeGen(getThreadLocalCodeGen()),
            _type(initialValue.type),
            _name(name)
    {
        size_t i = 0;
        for (llvm::Value * llvmValue : initialValue.getRawValues()) {
            _subNodes.emplace_back(
                    std::make_unique<PhiNode<llvm::Value>>(llvmValue, name + "_" + std::to_string(i)) // TODO Twine?
            );
            i += 1;
        }
    }

    PhiNode(PhiNode && other) :
        codeGen(other.codeGen),
        _type(other._type),
        _name(std::move(other._name)),
        _subNodes(std::move(other._subNodes))
    {
        other._subNodes.clear();
    }

    void addIncoming(const Sql::Value & value)
    {
        assert(Sql::equals(_type, value.type, Sql::SqlTypeEqualsMode::Full));

        size_t i = 0;
        for (llvm::Value * llvmValue : value.getRawValues()) {
            _subNodes[i]->addIncoming(llvmValue);
            i += 1;
        }
    }

    PhiNode & operator =(const Sql::Value & value)
    {
        addIncoming(value);
        return *this;
    }

    Sql::value_op_t get()
    {
        std::vector<llvm::Value *> rawValues;

        for (auto & subNode : _subNodes) {
            rawValues.push_back(subNode->get());
        }

        Sql::value_op_t sqlValue = Sql::Value::fromRawValues(rawValues, _type);
        return sqlValue;
    }

private:
    Sql::SqlType _type;

    std::string _name;
    std::vector<std::unique_ptr<PhiNode<llvm::Value>>> _subNodes;
};
