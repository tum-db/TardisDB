//
// Created by Blum Thomas on 2020-06-29.
//

#include "include/tardisdb/semanticAnalyser/SemanticalVerifier.hpp"

namespace semanticalAnalysis {

    // identifier -> (binding, Attribute)
    using scope_t = std::unordered_map<std::string, std::pair<std::string,ci_p_t>>;

    bool in_scope(const scope_t & scope, const tardisParser::BindingAttribute & binding_attr) {
        std::string identifier = binding_attr.first + "." + binding_attr.second;
        return scope.count(identifier) > 0;
    }

    scope_t construct_scope(Database& db, const tardisParser::SQLParserResult & result) {
        scope_t scope;
        for (auto & rel_pair : result.relations) {
            if (!db.hasTable(rel_pair.first)) {
                throw semantic_sql_error("unknown relation '" + rel_pair.first + "'");
            }
            auto * table = db.getTable(rel_pair.first);
            size_t attr_cnt = table->getColumnCount();
            for (size_t i = 0; i < attr_cnt; ++i) {
                auto * attr = table->getCI(table->getColumnNames()[i]);
                scope[rel_pair.second + "." + attr->columnName] = std::make_pair(rel_pair.second, attr);
                if (scope.count(attr->columnName) > 0) {
                    scope[attr->columnName].second = nullptr;
                } else {
                    scope[attr->columnName] = std::make_pair(rel_pair.second, attr);
                }
            }
        }
        return scope;
    }

    tardisParser::BindingAttribute fully_qualify(const tardisParser::BindingAttribute & current, const scope_t & scope) {
        if (!current.first.empty()) {
            return current;
        }
        auto it = scope.find(current.second);
        if (it == scope.end()) {
            throw semantic_sql_error("unknown column '" + current.second + "'");
        }
        return tardisParser::BindingAttribute(it->second.first, current.second);
    }

    void fully_qualify_names(const scope_t & scope, tardisParser::SQLParserResult & result) {
        for (size_t i = 0; i < result.selections.size(); ++i) {
            auto & [binding_attr, _] = result.selections[i];
            result.selections[i].first = fully_qualify(binding_attr, scope);
        }

        for (size_t i = 0; i < result.joinConditions.size(); ++i) {
            auto & [lhs, rhs] = result.joinConditions[i];
            result.joinConditions[i].first = fully_qualify(lhs, scope);
            result.joinConditions[i].second = fully_qualify(rhs, scope);
        }

        // TODO same for result.projections
    }

    void validate_sql_statement(const scope_t & scope, Database& db, const tardisParser::SQLParserResult & result) {
        for (auto & attr_name : result.projections) {
            auto it = scope.find(attr_name);
            if (it == scope.end()) {
                throw semantic_sql_error("unknown column '" + attr_name + "'");
            } else if (it->second.second == nullptr) {
                throw semantic_sql_error("'" + attr_name + "' is ambiguous");
            }
        }

        for (auto & selection_pair : result.selections) {
            auto & binding_attr = selection_pair.first;
            if (!in_scope(scope, binding_attr)) {
                throw semantic_sql_error("unknown column '" + binding_attr.first + "." + binding_attr.second + "'");
            }
        }

        for (auto & join_pair : result.joinConditions) {
            auto & lhs = join_pair.first;
            if (!in_scope(scope, lhs)) {
                throw semantic_sql_error("unknown column '" + lhs.first + "." + lhs.second + "'");
            }
            auto & rhs = join_pair.second;
            if (!in_scope(scope, rhs)) {
                throw semantic_sql_error("unknown column '" + rhs.first + "." + rhs.second + "'");
            }
        }
    }

    void SemanticalVerifier::analyse_sql_statement(tardisParser::SQLParserResult &result) {
        auto scope = construct_scope(_db, result);
        fully_qualify_names(scope, result);
        validate_sql_statement(scope, _db, result);
    }

}