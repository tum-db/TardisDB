
#include "semantic_analysis.hpp"

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

#include "queryparser.hpp"
#include "algebra/TableScan.hpp"
#include "algebra/Selection.hpp"
#include "algebra/HashJoin.hpp"
#include "algebra/Print.hpp"

//#include "generated/tables.hpp"
#include "Database.hpp"

using namespace Algebra;
using namespace QueryParser;

/*
Implement semantic analysis, which takes the parsed SQL statement and the schema as input and computes an algebra tree that can be used as input to your code generator. Report errors when non-existing attributes or tables are referenced. Also, you should report an error if a table has no join condition (i.e., a cross product would be necessary). Build left-deep join trees based on the order of the relations in the from clause.
*/
std::unique_ptr<Operator> computeTree(const ParsedQuery & query, QueryContext & context)
{
    if (query.where.empty() && query.from.size() > 1) {
        throw std::runtime_error("cross product required");
    }

    std::unordered_map<std::string, iu_p_t> scope;
    std::unordered_map<std::string, Table *> attributeToTable;
    std::unordered_map<Table *, std::unique_ptr<Operator>> scans;

    // create the table scan operators and build the scope
    for (const std::string & relation : query.from) {
//        Table * table = Database::getTable(relation);
        Table * table = context.db->getTable(relation);
        if (table == nullptr) {
            throw std::runtime_error("Relation '" + relation + "' doesn't exist");
        }

        TableScan * scan = new TableScan(context, *table);

        for (const std::string & column : table->getColumnNames()) {
//            scope[column] = scan->getIU(column);
            auto ci = table->getCI(column);
            scope[column] = context.iuFactory.getIU(column, ci->type);
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
            if (ci1->type == ci2->type) {
                throw std::runtime_error("incompatible types");
            }

            joinPairs.emplace_back(predicate.lhs, predicate.rhs);
        } else {
            // no it is a selection

            iu_p_t attr;
            Table * table;
            std::string constant;

            // determine the constant and the affected relation
            if (predicate.lhsType == ExpressionType::Attribute &&
                    predicate.rhsType == ExpressionType::Constant)
            {
                attr = scope[predicate.lhs];
                table = attributeToTable[predicate.lhs];
                constant = predicate.rhs;
            } else if (predicate.lhsType == ExpressionType::Constant &&
                    predicate.rhsType == ExpressionType::Attribute)
            {
                attr = scope[predicate.rhs];
                table = attributeToTable[predicate.rhs];
                constant = predicate.lhs;
            } else {
                throw std::runtime_error("invalid predicate");
            }

            // construct the selection operator
            auto selection = std::make_unique<Selection>( std::move(scans[table]), attr, Selection::Equals, constant );
            scans[table] = std::move(selection);
        }
    }

    // are there any joins involved?
    if (query.from.size() == 1) {
        return std::make_unique<Print>( std::move(scans.begin()->second), select );
    }

//    Table * firstTable = Database::getTable(query.from.front());
    Table * firstTable = context.db->getTable(query.from.front());
    std::unique_ptr<Operator> root = std::move( scans[firstTable] );

    // join
    std::deque<Table *> toJoin;
    std::set<Table *> joined = { firstTable };

    // order of the relations in the from clause
    for (auto it = std::next(query.from.begin()); it != query.from.end(); ++it) {
//        Table * table = Database::getTable(*it);
        Table * table = context.db->getTable(*it);
        toJoin.push_back(table);
    }

    // construct the left deep join plan
    size_t visited = 0;
    while (!toJoin.empty()) {
        const auto rel = toJoin.front(); // right branch
        join_pair_vec_t joinCondition;

        auto it = joinPairs.begin();
        while (it != joinPairs.end()) {
            const auto & joinPair = *it;

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

            // join this branch with the existing tree
            auto join = std::make_unique<HashJoin>(std::move(root), std::move(scans[rel]), joinCondition);
            root = std::move(join);
        }
    }

    auto print = std::make_unique<Print>(std::move(root), select);
    return print;
}
