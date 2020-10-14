//
// Created by Blum Thomas on 2020-05-21.
//

#ifndef PROTODB_DELETE_HPP
#define PROTODB_DELETE_HPP

#include "algebra/physical/Operator.hpp"

namespace Algebra {
    namespace Physical {

        class Delete : public UnaryOperator {
        public:
            Delete(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, iu_p_t &tidIU, Table & table);

            virtual ~Delete();

            void produce() override;
            void consume(const iu_value_mapping_t & values, const Operator & src) override;

        private:
            Table & table;
            llvm::Value * tupleCountPtr;

            iu_p_t tidIU;
        };
    } // end namespace Physical
} // end namespace Algebra


#endif //PROTODB_DELETE_HPP
