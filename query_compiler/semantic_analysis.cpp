#include "query_compiler/semantic_analysis.hpp"

#include <cstdlib>
#include <set>
#include <deque>
#include <list>
#include <unordered_map>
#include <iostream>
#include <iterator>
#include <sstream>
#include <fstream>
#include <memory>

#include "foundations/Database.hpp"
#include "query_compiler/queryparser.hpp"

using namespace Algebra::Logical;
using namespace QueryParser;

/*
Implement semantic analysis, which takes the parsed SQL statement and the
schema as input and computes an algebra tree that can be used as input to your
code generator. Report errors when non-existing attributes or tables are
referenced. Also, you should report an error if a table has no join condition
(i.e., a cross product would be necessary). Build left-deep join trees based
on the order of the relations in the from clause.
*/
std::unique_ptr<Result> computeTree(const ParsedQuery & query, QueryContext & context)
{
    if (query.where.empty() && query.from.size() > 1) {
        throw std::runtime_error("cross product required");
    }

    std::unordered_map<std::string, iu_p_t> scope;
    std::unordered_map<std::string, Table *> attributeToTable; // FIXME handle ambiguous names
    std::unordered_map<Table *, std::unique_ptr<Operator>> scans;

    // create the table scan operators and build the scope
    for (const std::string & relation : query.from) {
        auto * table = context.db.getTable(relation);
        if (table == nullptr) {
            throw std::runtime_error("Relation '" + relation + "' doesn't exist");
        }

        auto * scan = new TableScan(context, *table);

        for (const std::string & column : table->getColumnNames()) {
            auto ci = table->getCI(column);
            scope[column] = context.iuFactory.createIU(*scan, ci);
            attributeToTable[column] = table;
        }

        scans[table] = std::unique_ptr<Operator>(scan);
    }

    // resolve the column names to information units
    std::vector<iu_p_t> select;
    for (const std::string & attr : query.select) {
        if (attributeToTable.find(attr) == attributeToTable.end()) {
            throw std::runtime_error("column " + attr + " not in scope");
        }

        select.push_back( scope[attr] );
    }

    using ExpressionType = ParsedQuery::Predicate::TokenType;

    // collect all join pairs which are identified by their corresponding attributes
    std::list<std::pair<std::string, std::string>> joinPairs;
    for (const auto & predicate : query.where) {
        auto it1 = attributeToTable.find(predicate.lhs);
        if (predicate.lhsType == ExpressionType::Attribute && it1 == attributeToTable.end()) {
            throw std::runtime_error("column " + predicate.lhs + " not in scope");
        }

        auto it2 = attributeToTable.find(predicate.rhs);
        if (predicate.rhsType == ExpressionType::Attribute && it2 == attributeToTable.end()) {
            throw std::runtime_error("column " + predicate.rhs + " not in scope");
        }

        // is this a join condition?
        if (predicate.lhsType == ExpressionType::Attribute && predicate.rhsType == ExpressionType::Attribute) {
            Table * table1 = it1->second;
            Table * table2 = it2->second;

            auto * ci1 = table1->getCI(predicate.lhs);
            auto * ci2 = table2->getCI(predicate.rhs);

            // TODO upgrade to compare all compatible types
            if (ci1->type.typeID != ci2->type.typeID) { // FIXME: not very robust
                throw std::runtime_error("incompatible types");
            }

            joinPairs.emplace_back(predicate.lhs, predicate.rhs);
        } else {
            // no it is a select

            iu_p_t attr;
            Table * table;
            std::string value;
            ci_p_t ci;

            // determine the constant and the affected relation
            if (predicate.lhsType == ExpressionType::Attribute &&
                    predicate.rhsType == ExpressionType::Constant)
            {
                attr = scope[predicate.lhs];
                table = attributeToTable[predicate.lhs];
                value = predicate.rhs;
                ci = table->getCI(predicate.lhs);
            } else if (predicate.lhsType == ExpressionType::Constant &&
                    predicate.rhsType == ExpressionType::Attribute)
            {
                attr = scope[predicate.rhs];
                table = attributeToTable[predicate.rhs];
                value = predicate.lhs;
                ci = table->getCI(predicate.rhs);
            } else {
                throw std::runtime_error("invalid predicate");
            }

            // build the expression
            if (ci->type.nullable) {
                throw NotImplementedException();
            }
            auto constExp = std::make_unique<Expressions::Constant>(value, ci->type);
            auto identifier = std::make_unique<Expressions::Identifier>(attr);
            // TODO auto cast
            auto exp = std::make_unique<Expressions::Comparison>(
                Expressions::ComparisonMode::eq,
                std::move(identifier),
                std::move(constExp)
            );

            // construct the select operator
            auto selection = std::make_unique<Select>(std::move(scans[table]), std::move(exp));
            scans[table] = std::move(selection);
        }
    }

    // are there any joins involved?
    if (query.from.size() == 1) {
        return std::make_unique<Result>( std::move(scans.begin()->second), select );
    }

    auto * firstTable = context.db.getTable(query.from.front());
    std::unique_ptr<Operator> root = std::move( scans[firstTable] );

    // join
    std::deque<Table *> toJoin;
    std::set<Table *> joined = { firstTable };

    // order of the relations in the from clause
    for (auto it = std::next(query.from.begin()); it != query.from.end(); ++it) {
        auto * table = context.db.getTable(*it);
        toJoin.push_back(table);
    }

    // construct the left deep join plan
    size_t visited = 0;
    while (!toJoin.empty()) {
        const auto rel = toJoin.front(); // right branch
        using join_pair_vec_t = std::vector<std::pair<iu_p_t, iu_p_t>>;
        join_pair_vec_t joinCondition;

        auto it = joinPairs.begin();
        while (it != joinPairs.end()) {
            const auto & joinPair = *it;

            // TODO handle ambiguous names
            if (attributeToTable[joinPair.first] == rel &&
                    joined.count(attributeToTable[joinPair.second]) > 0)
            {
                // the order of the iu's has to match the tree structure
                joinCondition.emplace_back( scope[joinPair.second], scope[joinPair.first] );
                joined.insert(rel);
                it = joinPairs.erase(it);
            } else if (attributeToTable[joinPair.second] == rel &&
                    joined.count(attributeToTable[joinPair.first]) > 0)
            {
                joinCondition.emplace_back( scope[joinPair.first], scope[joinPair.second] );
                joined.insert(rel);
                it = joinPairs.erase(it);
            } else {
                it++;
            }
        }

        // check if this pair has a join condition
        if (joinCondition.empty()) {
            if (visited == toJoin.size()) {
                throw std::runtime_error("cross product required");
            } else {
                // fallback:
                toJoin.pop_front();
                toJoin.push_back(rel);
                visited += 1;
            }
        } else {
            toJoin.pop_front();
            visited = 0; // reset

            // construct the join expression
            std::unique_ptr<Expressions::Expression> joinExp;
            for (auto iuPair : joinCondition) {
                auto subExp = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq, // equijoin
                    std::make_unique<Expressions::Identifier>(iuPair.first),
                    std::make_unique<Expressions::Identifier>(iuPair.second)
                );
                if (joinExp) {
                    // construct implicit 'and' condition
                    auto andExp = std::make_unique<Expressions::And>(
                        std::move(joinExp),
                        std::move(subExp)
                    );
                    joinExp = std::move(andExp);
                } else {
                    joinExp = std::move(subExp);
                }
            }

            // join this branch with the existing tree
            auto join = std::make_unique<Join>(
                std::move(root),
                std::move(scans[rel]),
                std::move(joinExp),
                Join::Method::Hash
            );
            root = std::move(join);
        }
    }

    auto print = std::make_unique<Result>(std::move(root), select);
    return print;
}
