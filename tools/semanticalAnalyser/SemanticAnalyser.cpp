#include "semanticAnalyser/SemanticAnalyser.hpp"

#include "foundations/Database.hpp"
#include "native/sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include "semanticAnalyser/SemanticalVerifier.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>
#include <native/sql/SqlTuple.hpp>

namespace semanticalAnalysis {

    void SemanticAnalyser::construct_scans(AnalyzingContext& context, std::vector<Relation> &relations) {
        for (auto &relation : relations) {
            if (relation.alias.compare("") == 0) relation.alias = relation.name;

            // Recognize version
            std::string &branchName = relation.version;
            branch_id_t branchId;
            if (branchName.compare("master") != 0) {
                branchId = context.db._branchMapping.at(branchName);
            } else {
                branchId = master_branch_id;
            }
            context.branchIds.insert(branchId);

            //Construct the logical TableScan operator
            Table* table = context.db.getTable(relation.name);
            std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context.iuFactory, *table, branchId);

            //Store the ius produced by this TableScan; only the columns of the branch have names
            const auto &schema = table->getSchema(branchId);
            for (iu_p_t iu : scan->getProduced()) {
                ci_p_t ci = iu->columnInformation;
                context.scanIUs[relation.alias].push_back(iu);
                if (ci == table->getTIDColumnInformation().get() ||
                    std::find(schema.begin(), schema.end(), ci->index) != schema.end()) {
                    context.ius[relation.alias][ci->columnName] = iu;
                }
            }

            //Add a new production with TableScan as root node
            context.dangling_productions.insert({relation.alias, std::move(scan)});
        }
    }

    void SemanticAnalyser::construct_selects(AnalyzingContext& context, std::vector<std::pair<Column,std::string>> &selections) {
        for (auto &[column,valueString] : selections) {
            // Get iu
            iu_p_t iu;
            if (column.table.compare("") == 0) {
                for (auto &[productionName,production] : context.ius) {
                    for (auto &[key,value] : production) {
                        if (key.compare(column.name) == 0) {
                            column.table = productionName;
                            iu = value;
                        }
                    }
                }
            } else {
                iu = context.ius[column.table][column.name];
            }

            // an equality with a constant on an indexed column lets the scan read only the candidates of the index
            Operator * production = context.dangling_productions[column.table].get();
            while (auto * select = dynamic_cast<Select *>(production)) production = &select->getChild();
            auto * scan = dynamic_cast<TableScan *>(production);
            if (scan != nullptr && scan->getIndex() == nullptr && valueString != Native::Sql::nullLiteral) {
                Table & table = scan->getTable();
                if (const BranchIndex * index = table.getIndex(iu->columnInformation->index, scan->getBranchId())) {
                    auto key = Native::Sql::Value::castString(valueString, Sql::toNotNullableTy(iu->columnInformation->type));
                    scan->setIndexProbe(index, key->hash());
                }
            }

            // Construct Expression
            auto constExp = std::make_unique<Expressions::Constant>(valueString, iu->columnInformation->type);
            auto identifier = std::make_unique<Expressions::Identifier>(iu);
            std::unique_ptr<Expressions::Comparison> exp = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq,
                    std::move(identifier),
                    std::move(constExp)
            );

            //Construct the logical Select operator
            std::unique_ptr<Select> select = std::make_unique<Select>(std::move(context.dangling_productions[column.table]), std::move(exp));

            //Update corresponding production by setting the Select operator as new root node
            context.dangling_productions[column.table] = std::move(select);
        }
    }

    ColumnResolver SemanticAnalyser::columnResolver(AnalyzingContext & context, std::set<std::string> * productions) {
        return [&context, productions](const std::string & table, const std::string & name) -> iu_p_t {
            iu_p_t iu = nullptr;
            std::string production = table;
            if (production.empty()) {
                for (auto & [productionName, columns] : context.ius) {
                    auto it = columns.find(name);
                    if (it != columns.end() && name != "tid") {
                        if (iu != nullptr) throw semantic_sql_error("column '" + name + "' is ambiguous");
                        iu = it->second;
                        production = productionName;
                    }
                }
            } else if (context.ius.count(production) > 0 && context.ius[production].count(name) > 0) {
                iu = context.ius[production][name];
            } else if (context.ius.count(production) == 0) {
                throw semantic_sql_error("binding '" + table + "' is not specified");
            }
            if (iu != nullptr && productions != nullptr) productions->insert(production);
            return iu;
        };
    }

    Expressions::exp_op_t SemanticAnalyser::predicateExpression(const Predicate & predicate, const std::vector<iu_p_t> & ius) {
        using Expressions::ComparisonMode;
        auto value = [&](size_t i, const std::string & v) {
            return std::make_unique<Expressions::Constant>(v, ius[i]->columnInformation->type);
        };
        auto comparison = [&](ComparisonMode mode, size_t i, const std::string & v) -> Expressions::exp_op_t {
            return std::make_unique<Expressions::Comparison>(mode, std::make_unique<Expressions::Identifier>(ius[i]), value(i, v));
        };
        auto modeOf = [](const std::string & op) {
            if (op == "<") return ComparisonMode::less;
            if (op == "<=") return ComparisonMode::leq;
            if (op == ">") return ComparisonMode::gtr;
            if (op == ">=") return ComparisonMode::geq;
            throw semantic_sql_error("unknown comparison '" + op + "'");
        };

        Expressions::exp_op_t exp;
        if (predicate.op == "is null" || predicate.op == "is not null") {
            exp = std::make_unique<Expressions::IsNull>(std::make_unique<Expressions::Identifier>(ius[0]), predicate.op == "is not null");
        } else if (predicate.op == "in") {
            for (auto & v : predicate.values) {
                auto eq = comparison(ComparisonMode::eq, 0, v);
                exp = exp ? std::make_unique<Expressions::Or>(std::move(exp), std::move(eq)) : std::move(eq);
            }
        } else if (predicate.op == "<>") {
            // not all equal
            for (size_t i = 0; i < ius.size(); ++i) {
                auto eq = comparison(ComparisonMode::eq, i, predicate.values[i]);
                exp = exp ? std::make_unique<Expressions::And>(std::move(exp), std::move(eq)) : std::move(eq);
            }
            exp = std::make_unique<Expressions::Not>(std::move(exp));
        } else {
            // lexicographic: c0 < v0 OR (c0 = v0 AND (c1 < v1 OR (c1 = v1 AND ... cn <= vn))) for <=
            ComparisonMode last = modeOf(predicate.op);
            ComparisonMode strict = (last == ComparisonMode::less || last == ComparisonMode::leq) ? ComparisonMode::less : ComparisonMode::gtr;
            size_t n = ius.size();
            exp = comparison(last, n - 1, predicate.values[n - 1]);
            for (size_t i = n - 1; i-- > 0;) {
                auto eqAndRest = std::make_unique<Expressions::And>(comparison(ComparisonMode::eq, i, predicate.values[i]), std::move(exp));
                exp = std::make_unique<Expressions::Or>(comparison(strict, i, predicate.values[i]), std::move(eqAndRest));
            }
        }
        return exp;
    }

    void SemanticAnalyser::construct_predicates(AnalyzingContext& context, std::vector<Predicate> &predicates) {
        for (auto & predicate : predicates) {
            if (predicate.op == "expr") {
                // any condition: over the production of its table, or over the joined tree
                std::set<std::string> productions;
                ExpressionAnalyser analyser(columnResolver(context, &productions));
                Expressions::exp_op_t exp = analyser.analyseCondition(*predicate.expression);
                if (productions.size() > 1 || context.dangling_productions.empty()) {
                    context.joinedPredicates.push_back(std::move(exp));
                    continue;
                }
                std::string table = productions.empty() ? context.dangling_productions.begin()->first : *productions.begin();
                context.dangling_productions[table] = std::make_unique<Select>(std::move(context.dangling_productions[table]), std::move(exp));
                continue;
            }
            // the columns, all of one table
            std::vector<iu_p_t> ius;
            std::string table;
            for (auto & column : predicate.columns) {
                iu_p_t iu = nullptr;
                std::string production = column.table;
                if (production.empty()) {
                    for (auto & [productionName, columns] : context.ius) {
                        auto it = columns.find(column.name);
                        if (it != columns.end()) {
                            if (iu != nullptr) throw semantic_sql_error("column '" + column.name + "' is ambiguous");
                            iu = it->second;
                            production = productionName;
                        }
                    }
                } else if (context.ius.count(production) > 0 && context.ius[production].count(column.name) > 0) {
                    iu = context.ius[production][column.name];
                }
                if (iu == nullptr) throw semantic_sql_error("column '" + column.name + "' does not exist");
                if (!table.empty() && table != production) throw semantic_sql_error("a row value compares columns of one table");
                table = production;
                ius.push_back(iu);
            }
            Expressions::exp_op_t exp = predicateExpression(predicate, ius);
            context.dangling_productions[table] = std::make_unique<Select>(std::move(context.dangling_productions[table]), std::move(exp));
        }
    }

    std::unique_ptr<SemanticAnalyser> SemanticAnalyser::getSemanticAnalyser(AnalyzingContext &context) {
        switch (context.parserResult.opType) {
            case SQLParserResult::OpType::Select:
                return std::make_unique<SelectAnalyser>(context);
            case SQLParserResult::OpType::Insert:
                return std::make_unique<InsertAnalyser>(context);
            case SQLParserResult::OpType::Update:
                return std::make_unique<UpdateAnalyser>(context);
            case SQLParserResult::OpType::Delete:
                return std::make_unique<DeleteAnalyser>(context);
            case SQLParserResult::OpType::CreateTable:
                return std::make_unique<CreateTableAnalyser>(context);
            case SQLParserResult::OpType::CreateBranch:
                return std::make_unique<CreateBranchAnalyser>(context);
            case SQLParserResult::OpType::Branch:
                return std::make_unique<BranchAnalyser>(context);
            case SQLParserResult::OpType::Copy:
                return std::make_unique<CopyTableAnalyser>(context);
            case SQLParserResult::OpType::CreateIndex:
                return std::make_unique<CreateIndexAnalyser>(context);
            case SQLParserResult::OpType::DropBranch:
                return std::make_unique<DropBranchAnalyser>(context);
            case SQLParserResult::OpType::MergeBranch:
                return std::make_unique<MergeBranchAnalyser>(context);
            case SQLParserResult::OpType::AlterTable:
                return std::make_unique<AlterTableAnalyser>(context);
            case SQLParserResult::OpType::Unknown:
                return nullptr;
        }

        return nullptr;
    }
}



