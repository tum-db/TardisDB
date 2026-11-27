#include "algebra/physical/Insert.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include "codegen/LLVMCompat.hpp"

#include <iostream>

#include "algebra/logical/operators.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "sql/SqlCasts.hpp"
#include "sql/ValueTranslator.hpp"

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Insert::Insert(const logical_operator_t & logicalOperator, Table & table, std::vector <Native::Sql::SqlTuple *> tuples, QueryContext &context, branch_id_t branchId) :
                NullaryOperator(std::move(logicalOperator),context) , table(table), tuples(move(tuples)), context(context), branchId(branchId)
        {

        }

        Insert::~Insert()
        { }

        void Insert::produce()
        {
            _context.executionContext.affectedRows = tuples.size();
            genInsertCall((void *)&insert_tuple_with_branchId);
        }

        void Insert::genInsertCall(void *funcPtr) {
            llvm::FunctionType * funcTy = llvm::TypeBuilder<tid_t (void *, void *, void *, branch_id_t), false>::get(_codeGen.getLLVMContext());
            llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "insert_tuple_with_binding", funcTy);
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,funcPtr);
            if (tuples.empty()) return;

            // a loop over the tuples, which outlive this operator in the execution context: one call
            // per tuple in straight-line code would take time quadratic in their number to compile
            auto array = std::make_unique<PointerArrayResource>();
            array->pointers.assign(tuples.begin(), tuples.end());
            void ** pointers = array->pointers.data();
            _context.executionContext.acquireResource(std::move(array));

            auto & llvmContext = _codeGen.getLLVMContext();
            llvm::Type * pointerTy = LLVMCompat::getInt8PtrTy(llvmContext);
            llvm::Value * base = _codeGen->CreatePointerCast(cg_ptr8_t::fromRawPointer(pointers), llvm::PointerType::getUnqual(pointerTy));
            cg_size_t count(tuples.size());
            auto & funcGen = _codeGen.getCurrentFunctionGen();
            LoopGen loop(funcGen, {{"tuple", cg_size_t(0ul)}});
            cg_size_t index(loop.getLoopVar(0));
            {
                LoopBodyGen body(loop);
                llvm::Value * tuple = _codeGen->CreateLoad(pointerTy, _codeGen->CreateGEP(pointerTy, base, index.getValue()));
                _codeGen->CreateCall(func, {tuple, cg_ptr8_t::fromRawPointer(&table), funcGen.getArg(1), cg_u32_t(branchId)});
            }
            cg_size_t next = index + 1ul;
            loop.loopDone(_codeGen->CreateICmpULT(next.getValue(), count.getValue()), {next});
        }

        /// The tuple the generated code fills for each row, as long as the execution
        struct InsertTupleResource : public ExecutionResource {
            std::unique_ptr<Native::Sql::SqlTuple> tuple;
        };

        /// Inserts the tuple into the branch and counts it
        static void insert_query_tuple(Native::Sql::SqlTuple * tuple, Table * table, QueryContext * context, branch_id_t branchId)
        {
            insert_tuple_with_branchId(*tuple, *table, *context, branchId);
            ++context->executionContext.affectedRows;
        }

        static void insert_query_null(const char * column)
        {
            throw semanticalAnalysis::not_null_violation("null value in column '" + std::string(column) + "', which is NOT NULL");
        }

        InsertQuery::InsertQuery(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & context) :
                UnaryOperator(logicalOperator, std::move(input), context)
        { }

        InsertQuery::~InsertQuery()
        { }

        void InsertQuery::produce()
        {
            child->produce();
        }

        void InsertQuery::consume(const iu_value_mapping_t & values, const Operator & src)
        {
            auto & logical = static_cast<const Logical::InsertQuery &>(_logicalOperator);
            // the values in the types of their columns, written into the tuple by the generated code
            std::vector<std::pair<ci_p_t, Native::Sql::value_op_t>> written;
            for (size_t c = 0; c < logical._values.size(); ++c) {
                ci_p_t ci = logical._columns[c];
                const Sql::Value & given = *values.at(logical._values[c]);
                if (given.type.typeID == SqlType::TypeID::UnknownID) {
                    // NULL: the default of the column
                    if (!ci->type.nullable) throw semanticalAnalysis::not_null_violation("null value in column '" + ci->columnName + "', which is NOT NULL");
                    continue;
                }
                value_op_t value = given.clone();
                if (!Sql::equals(value->type, ci->type, SqlTypeEqualsMode::WithoutNullable)) {
                    SqlType castType = ci->type;
                    castType.nullable = value->type.nullable;
                    value = Sql::Casts::cast(*value, castType, Sql::Casts::Context::Assignment);
                }
                if (!ci->type.nullable && Sql::isNullable(*value)) {
                    auto & nullable = static_cast<const NullableValue &>(*value);
                    {
                        IfGen isNull(nullable.isNull());
                        llvm::FunctionType * type = llvm::FunctionType::get(_codeGen->getVoidTy(), {cg_voidptr_t::getType()}, false);
                        _codeGen.CreateCall(&insert_query_null, type, {cg_voidptr_t::fromRawPointer(ci->columnName.c_str())});
                    }
                    value = nullable.getValue().clone();
                } else if (ci->type.nullable && !Sql::isNullable(*value)) {
                    value = NullableValue::create(*value, cg_bool_t(false));
                }
                written.emplace_back(ci, ValueTranslator::sqlValueToNativeSqlValue(value.get()));
            }
            // the other columns get their defaults
            auto resource = std::make_unique<InsertTupleResource>();
            resource->tuple = complete_tuple(logical._table, std::move(written));
            Native::Sql::SqlTuple * tuple = resource->tuple.get();
            _context.executionContext.acquireResource(std::move(resource));

            llvm::FunctionType * type = llvm::FunctionType::get(_codeGen->getVoidTy(),
                    {cg_voidptr_t::getType(), cg_voidptr_t::getType(), cg_voidptr_t::getType(), _codeGen->getInt32Ty()}, false);
            _codeGen.CreateCall(&insert_query_tuple, type, {cg_voidptr_t::fromRawPointer(tuple), cg_voidptr_t::fromRawPointer(&logical._table),
                                                            _codeGen.getCurrentFunctionGen().getArg(1), cg_u32_t(logical._branchId)});
        }

    } // end namespace Physical
} // end namespace Algebra

