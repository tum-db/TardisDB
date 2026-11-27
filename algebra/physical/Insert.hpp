#ifndef PROTODB_INSERT_HPP
#define PROTODB_INSERT_HPP


#include "algebra/physical/Operator.hpp"
#include "native/sql/SqlTuple.hpp"

namespace Algebra {
    namespace Physical {

/// The print operator
        class Insert : public NullaryOperator {
        public:
            Insert(const logical_operator_t & logicalOperator, Table & table, std::vector<Native::Sql::SqlTuple *>tuples, QueryContext &context, branch_id_t branchId);

            virtual ~Insert();

            void produce() override;

        private:
            std::vector <Native::Sql::SqlTuple *> tuples;
            Table & table;
            QueryContext &context;
            branch_id_t branchId;

            void genInsertCall(void *funcPtr);
        };

/// INSERT of the rows of a query (see Logical::InsertQuery): the generated code writes each row into a tuple of the table and inserts it
        class InsertQuery : public UnaryOperator {
        public:
            InsertQuery(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & context);

            ~InsertQuery() override;

            void produce() override;

            void consume(const iu_value_mapping_t & values, const Operator & src) override;
        };

    } // end namespace Physical
} // end namespace Algebra


#endif //PROTODB_INSERT_HPP
