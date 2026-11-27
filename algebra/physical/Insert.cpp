#include "algebra/physical/Insert.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include "codegen/LLVMCompat.hpp"

#include <deque>
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

        /// The tuple the generated code fills for each row, and copies of the rows: an error of the query leaves the table as it was
        struct InsertRows : public ExecutionResource {
            std::unique_ptr<Native::Sql::SqlTuple> tuple;
            std::vector<std::unique_ptr<Native::Sql::SqlTuple>> rows;
            std::deque<std::string> strings; // the characters of the varchar and char values of the rows
        };

        static void insert_query_row(InsertRows * rows)
        {
            std::vector<Native::Sql::value_op_t> values;
            values.reserve(rows->tuple->values.size());
            for (auto & value : rows->tuple->values) {
                // a varchar points to characters the next row overwrites
                const Native::Sql::Value & plain = Native::Sql::getAssociatedValue(*value);
                const bool null = Native::Sql::isNullable(*value) && static_cast<const Native::Sql::NullableValue &>(*value).isNull();
                if (!null && (plain.type.typeID == SqlType::TypeID::VarcharID || plain.type.typeID == SqlType::TypeID::CharID)) {
                    rows->strings.push_back(Native::Sql::toString(plain));
                    values.push_back(Native::Sql::toColumnValue(Native::Sql::Value::castString(rows->strings.back(), plain.type), value->type));
                } else {
                    values.push_back(value->clone());
                }
            }
            rows->rows.push_back(std::make_unique<Native::Sql::SqlTuple>(std::move(values)));
        }

        /// Inserts the rows into the branch, after the query
        static void insert_query_rows(InsertRows * rows, Table * table, QueryContext * context, branch_id_t branchId)
        {
            for (auto & row : rows->rows) insert_tuple_with_branchId(*row, *table, *context, branchId);
            context->executionContext.affectedRows = rows->rows.size();
            rows->rows.clear();
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
            auto & logical = static_cast<const Logical::InsertQuery &>(_logicalOperator);
            auto rows = std::make_unique<InsertRows>();
            _rows = rows.get();
            _context.executionContext.acquireResource(std::move(rows));
            child->produce();
            llvm::FunctionType * type = llvm::FunctionType::get(_codeGen->getVoidTy(),
                    {cg_voidptr_t::getType(), cg_voidptr_t::getType(), cg_voidptr_t::getType(), _codeGen->getInt32Ty()}, false);
            _codeGen.CreateCall(&insert_query_rows, type, {cg_voidptr_t::fromRawPointer(_rows), cg_voidptr_t::fromRawPointer(&logical._table),
                                                           _codeGen.getCurrentFunctionGen().getArg(1), cg_u32_t(logical._branchId)});
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
            _rows->tuple = complete_tuple(logical._table, std::move(written));
            llvm::FunctionType * type = llvm::FunctionType::get(_codeGen->getVoidTy(), {cg_voidptr_t::getType()}, false);
            _codeGen.CreateCall(&insert_query_row, type, std::vector<llvm::Value *>{cg_voidptr_t::fromRawPointer(_rows)});
        }

    } // end namespace Physical
} // end namespace Algebra

