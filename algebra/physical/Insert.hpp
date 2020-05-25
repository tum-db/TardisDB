#ifndef PROTODB_INSERT_HPP
#define PROTODB_INSERT_HPP


#include "algebra/physical/Operator.hpp"
#include "native/sql/SqlTuple.hpp"

namespace Algebra {
    namespace Physical {

/// The print operator
        class Insert : public NullaryOperator {
        public:
            Insert(const logical_operator_t & logicalOperator, Table & table, Native::Sql::SqlTuple *tuple, QueryContext &context);

            virtual ~Insert();

            void produce() override;

        private:
            Native::Sql::SqlTuple *tuple;
            Table & table;
            QueryContext &context;
        };

    } // end namespace Physical
} // end namespace Algebra


#endif //PROTODB_INSERT_HPP
