#ifndef PROTODB_UPDATE_HPP
#define PROTODB_UPDATE_HPP

#include "algebra/physical/Operator.hpp"

namespace Algebra {
    namespace Physical {

/// The print operator
        class Update : public UnaryOperator {
        public:
            Update(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, Table & table,
                    std::vector<std::pair<iu_p_t,std::string>> &updateIUs, std::string *alias);

            virtual ~Update();

            void produce() override;
            void consume(const iu_value_mapping_t & values, const Operator & src) override;

        private:
            std::vector<std::pair<iu_p_t,std::string>> _updateIUs;
            Table & table;
            std::string *alias;
            llvm::Value * tupleCountPtr;

            using column_t = std::tuple<ci_p_t, llvm::Type *, llvm::Value *, size_t, std::unique_ptr<Sql::Value>>;
            std::vector<column_t> columns;

            static bool sortColumns(column_t& a, column_t& b) {
                return (std::get<3>(a) < std::get<3>(b));
            }
        };

    } // end namespace Physical
} // end namespace Algebra

#endif //PROTODB_UPDATE_HPP
