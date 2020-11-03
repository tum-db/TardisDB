//
// Created by Blum Thomas on 2020-06-16.
//

#ifndef PROTODB_TUPLESTREAM_HPP
#define PROTODB_TUPLESTREAM_HPP

#include "algebra/physical/Operator.hpp"

namespace Algebra {
    namespace Physical {

/// The print operator
        class TupleStream : public UnaryOperator {
        public:
            TupleStream(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext &queryContext);

            virtual ~TupleStream();

            void produce() override;
            void consume(const iu_value_mapping_t & values, const Operator & src) override;

        private:
            std::vector<iu_p_t> selection;

            void genCallbackCall(Native::Sql::SqlTuple* nativetuple);

            using column_t = std::tuple<iu_p_t>;
            std::vector<column_t> columns;

            llvm::Value * tupleCountPtr;
        };

    } // end namespace Physical
} // end namespace Algebra


#endif //PROTODB_TUPLESTREAM_HPP
