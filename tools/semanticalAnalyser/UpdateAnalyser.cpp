//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void UpdateAnalyser::verify() {
        Database &db = _context.db;
        UpdateStatement* stmt = _context.parserResult.updateStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name)) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
        Table *table = db.getTable(stmt->relation.name);
        std::vector<std::string> columnNames = table->getColumnNames();
        for (auto &column : stmt->updates) {
            if (std::find(columnNames.begin(),columnNames.end(),column.first.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.first.name + "' does not exist");
        }
        for (auto &expression : stmt->expressions) {
            for (auto &term : expression) {
                if (term.column.name.empty()) continue;
                if (!term.column.table.empty() && term.column.table != stmt->relation.name && term.column.table != stmt->relation.alias)
                    throw semantic_sql_error("binding '" + term.column.table + "' is not specified");
                if (std::find(columnNames.begin(),columnNames.end(),term.column.name) == columnNames.end())
                    throw semantic_sql_error("column '" + term.column.name + "' does not exist");
            }
        }
        for (auto &column : stmt->selections) {
            if (std::find(columnNames.begin(),columnNames.end(),column.first.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.first.name + "' does not exist");
        }
        // Relation
        // // Relation with name exists?
        // // Branch exists?
        // For each column
        // // Column exists?
    }

    //TODO: Verifier check on only one table to update
    void UpdateAnalyser::constructTree() {
        UpdateStatement *stmt = _context.parserResult.updateStmt;

        construct_scans(_context, stmt->relation);
        construct_selects(_context, stmt->selections);

        Table* table = _context.db.getTable(stmt->relation.name);
        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;

        branch_id_t branchId = (stmt->relation.version.compare("master") != 0) ?
            _context.db._branchMapping.at(stmt->relation.version) :
            master_branch_id;

        //Check for every IU if an update is specified and set the update value to an empty string if not
        std::vector<std::pair<iu_p_t,std::string>> updateIUs;
        for (auto &iu : _context.ius[stmt->relation.alias]) {
            bool shouldBeUpdated = false;
            for (auto &[column,value] : stmt->updates) {
                shouldBeUpdated = iu.first.compare(column.name) == 0;
                if (shouldBeUpdated) {
                    updateIUs.emplace_back( iu.second,value);
                    break;
                }
            }
            if (!shouldBeUpdated) updateIUs.emplace_back( iu.second,"");
        }

        auto &production = _context.dangling_productions[stmt->relation.alias];
        auto update = std::make_unique<Update>( std::move(production), updateIUs, *table, branchId);

        // SET c = <terms>: constants take the type of the column; '*' and '/' bind before '+' and '-'
        auto &ius = _context.ius[stmt->relation.alias];
        for (size_t i = 0; i < stmt->updates.size(); ++i) {
            auto &terms = stmt->expressions[i];
            if (terms.size() == 1 && terms.front().column.name.empty()) continue; // a constant
            iu_p_t target = ius[stmt->updates[i].first.name];
            Sql::SqlType constantType = Sql::toNotNullableTy(target->columnInformation->type);
            auto operand = [&](const UpdateTerm &term) -> Expressions::exp_op_t {
                if (term.column.name.empty()) return std::make_unique<Expressions::Constant>(term.value, constantType);
                return std::make_unique<Expressions::Identifier>(ius[term.column.name]);
            };
            std::vector<std::pair<std::string, Expressions::exp_op_t>> sums; // operator and product
            for (auto &term : terms) {
                if (term.op == "*" || term.op == "/") {
                    auto &product = sums.back().second;
                    if (term.op == "*") product = std::make_unique<Expressions::Multiplication>(std::move(product), operand(term));
                    else product = std::make_unique<Expressions::Division>(std::move(product), operand(term));
                } else {
                    sums.emplace_back(term.op, operand(term));
                }
            }
            Expressions::exp_op_t expression = std::move(sums.front().second);
            for (size_t j = 1; j < sums.size(); ++j) {
                if (sums[j].first == "+") expression = std::make_unique<Expressions::Addition>(std::move(expression), std::move(sums[j].second));
                else expression = std::make_unique<Expressions::Subtraction>(std::move(expression), std::move(sums[j].second));
            }
            update->addUpdateExpression(target, std::move(expression));
        }
        _context.joinedTree = std::move(update);
    }

}