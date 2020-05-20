#ifndef PROTODB_UPDATE_HPP
#define PROTODB_UPDATE_HPP

#include "algebra/physical/Operator.hpp"

namespace Algebra {
    namespace Physical {

/// The print operator
        class Update : public UnaryOperator {
        public:
            Update(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, Table & table,
                    std::vector<iu_p_t> &updateIUs, std::vector<std::unique_ptr<Sql::Value>> &updateValues);

            virtual ~Update();

            void produce() override;
            void consume(const iu_value_mapping_t & values, const Operator & src) override;

        private:
            std::vector<iu_p_t> _updateIUs;
            std::vector<std::unique_ptr<Sql::Value>> _updateValues;
            Table & table;
            llvm::Value * tupleCountPtr;

            using column_t = std::tuple<ci_p_t, llvm::Type *, llvm::Value *>;
            std::vector<column_t> columns;
        };

    } // end namespace Physical
} // end namespace Algebra

#endif //PROTODB_UPDATE_HPP
