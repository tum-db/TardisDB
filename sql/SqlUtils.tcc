
namespace Sql {
namespace Utils {

template<typename T>
struct PhiNodeSelector;

#ifndef USE_INTERNAL_NULL_INDICATOR
inline value_op_t createNullValue(value_op_t value)
{
    assert(!value->type.nullable);
    return NullableValue::create(std::move(value), cg_bool_t(false));
}
#else
inline value_op_t createNullValue(value_op_t value)
{
    assert(!value->type.nullable);
    return NullableValue::create(std::move(value));
}
#endif // USE_INTERNAL_NULL_INDICATOR

//#define USE_LLVM_SELECT
#ifndef USE_LLVM_SELECT
template<typename F>
auto nullHandlerExtended(F presentFunc, F absentFunc, const Value & value) -> decltype(presentFunc(value))
{
    // compile-time null check
    if (isUnknown(value)) {
        return absentFunc(value);
    } else if (!isNullable(value)) {
        return presentFunc(value);
    }

    // runtime null check
    cg_bool_t isNull(false);
    if (isNullable(value)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(value);
        isNull = nullableValue.isNull();
    }

    auto defaultValue = absentFunc(value);
    // FIXME works only with llvm::Value *
    PhiNode< typename std::remove_pointer<decltype(presentFunc(value))>::type > resultNode(defaultValue, "resultNode");

    IfGen notNullCheck(!isNull);
    {
        auto opResult = presentFunc(getAssociatedValue(value));
        resultNode = opResult;
    }
    notNullCheck.EndIf();

    return resultNode.get();
}

template<typename ActionFun>
value_op_t nullHandler(ActionFun action, const Value & a, SqlType resultTy)
{
    // compile-time null check
    if (isUnknown(a)) {
        return NullableValue::getNull(a.type);
    } else if (!isNullable(a)) {
        return action(a);
    }

    // runtime null check
    cg_bool_t isNull(false);
    if (isNullable(a)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(a);
        isNull = nullableValue.isNull();
    }

    auto defaultValue = NullableValue::getNull(resultTy);
    PhiNode<Value> resultNode(*defaultValue, "resultNode");

    IfGen notNullCheck(!isNull);
    {
        value_op_t opResult = action(getAssociatedValue(a));

        // the attribute "nullable" is viral
        value_op_t nullableOpResult = createNullValue(std::move(opResult));
        resultNode = *nullableOpResult;
    }
    notNullCheck.EndIf();

    return resultNode.get();
}

template<typename ActionFun>
value_op_t nullHandler(ActionFun action, const Value & a, const Value & b, SqlType resultTy)
{
    // compile-time null check
    if (isUnknown(a) || isUnknown(b)) {
        return NullableValue::getNull(a.type);
    } else if (!isNullable(a) && !isNullable(b)) {
        return action(a, b);
    }

    // runtime null check
    cg_bool_t isNull(false);
    if (isNullable(a)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(a);
        isNull = isNull || nullableValue.isNull();
    }

    if (isNullable(b)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(b);
        isNull = isNull || nullableValue.isNull();
    }

    auto defaultValue = NullableValue::getNull(resultTy);
    PhiNode<Value> resultNode(*defaultValue, "resultNode");

    IfGen notNullCheck(!isNull);
    {
        value_op_t opResult = action(getAssociatedValue(a), getAssociatedValue(b));

        // the attribute "nullable" is viral
        value_op_t nullableOpResult = createNullValue(std::move(opResult));
        resultNode = *nullableOpResult;
    }
    notNullCheck.EndIf();

    return resultNode.get();
}

#else // USE_LLVM_SELECT

template<typename F>
auto nullHandlerExtended(F presentFunc, F absentFunc, const Value & value) -> decltype(presentFunc(value))
{
    // compile-time null check
    if (isUnknown(value)) {
        return absentFunc(value);
    } else if (!isNullable(value)) {
        return presentFunc(value);
    }

    // runtime null check
    cg_bool_t isNull(false);
    if (isNullable(value)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(value);
        isNull = nullableValue.isNull();
    }

    auto & codeGen = getThreadLocalCodeGen();

    // FIXME works only with llvm::Value *
    llvm::Value * defaultValue = absentFunc(value);
    llvm::Value * opResult = presentFunc(getAssociatedValue(value));
    return codeGen->CreateSelect(isNull, defaultValue, opResult);
}

template<typename ActionFun>
value_op_t nullHandler(ActionFun action, const Value & a, SqlType resultTy)
{
    // compile-time null check
    if (isUnknown(a)) {
        return NullableValue::getNull(a.type);
    } else if (!isNullable(a)) {
        return action(a);
    }

    // runtime null check
    cg_bool_t isNull(false);
    if (isNullable(a)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(a);
        isNull = nullableValue.isNull();
    }

    value_op_t defaultValue = NullableValue::getNull(resultTy);
    value_op_t opResult = action(getAssociatedValue(a));

    // the attribute "nullable" is viral
    value_op_t nullableOpResult = createNullValue(std::move(opResult));

    return genSelect(isNull, *defaultValue, *nullableOpResult);
}

template<typename ActionFun>
value_op_t nullHandler(ActionFun action, const Value & a, const Value & b, SqlType resultTy)
{
    // compile-time null check
    if (isUnknown(a) || isUnknown(b)) {
        return NullableValue::getNull(a.type);
    } else if (!isNullable(a) && !isNullable(b)) {
        return action(a, b);
    }

    // runtime null check
    cg_bool_t isNull(false);
    if (isNullable(a)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(a);
        isNull = isNull || nullableValue.isNull();
    }

    if (isNullable(b)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(b);
        isNull = isNull || nullableValue.isNull();
    }

    value_op_t defaultValue = NullableValue::getNull(resultTy);
    value_op_t opResult = action(getAssociatedValue(a), getAssociatedValue(b));

    // the attribute "nullable" is viral
    value_op_t nullableOpResult = createNullValue(std::move(opResult));

    return genSelect(isNull, *defaultValue, *nullableOpResult);
}
#endif // USE_LLVM_SELECT

} // end namespace Utils
} // end namespace Sql
