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
                branchId = context.db._branchMapping[branchName];
            } else {
                branchId = master_branch_id;
            }
            context.branchIds.insert(branchId);

            //Construct the logical TableScan operator
            Table* table = context.db.getTable(relation.name);
            std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context.iuFactory, *table, branchId);

            //Store the ius produced by this TableScan
            for (iu_p_t iu : scan->getProduced()) {
                context.ius[relation.alias][iu->columnInformation->columnName] = iu;
            }

            //Add a new production with TableScan as root node
            context.dangling_productions.insert({relation.alias, std::move(scan)});
        }
    }

    // TODO: Implement nullable
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
            case SQLParserResult::OpType::Unknown:
                return nullptr;
        }

        return nullptr;
    }
}



