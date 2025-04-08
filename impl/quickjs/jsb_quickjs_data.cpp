#include "jsb_quickjs_data.h"
#include "jsb_quickjs_isolate.h"
#include "jsb_quickjs_ext.h"

namespace v8
{
    int Data::GetIdentityHash() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        jsb_check(JS_VALUE_GET_TAG(val) < 0);

        const uintptr_t ptr = (uintptr_t) JS_VALUE_GET_PTR(val);
#if INTPTR_MAX >= INT64_MAX
        return (int) ((ptr >> 32) ^ (ptr & 0xffffffff));
#elif INTPTR_MAX >= INT32_MAX
        return (int) ptr;
#else
        #error "quickjs.impl does not support on the current arch"
#endif
    }

    Data::operator JSValue() const
    {
        return isolate_->stack_val(stack_pos_);
    }

    bool Data::IsNullOrUndefined() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsNull(val) || JS_IsUndefined(val);
    }

    bool Data::IsNull() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsNull(val);
    }

    bool Data::IsUndefined() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsUndefined(val);
    }

    bool Data::IsObject() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsObject(val);
    }

    bool Data::IsPromise() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsPromise(val);
    }

    bool Data::IsArray() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsArray(isolate_->ctx(), val);
    }

    bool Data::IsMap() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);

        //NOTE quickjs source modified
        return JS_IsMap(val);
    }

    bool Data::IsSymbol() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsSymbol(val);
    }

    bool Data::IsString() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsString(val);
    }

    bool Data::IsFunction() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsFunction(isolate_->ctx(), val);
    }

    bool Data::IsInt32() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);

        //TODO we can not determine whether it's int32 or uint32
        return JS_VALUE_GET_TAG(val) == JS_TAG_INT;
    }

    bool Data::IsUint32() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);

        //TODO we can not determine whether it's int32 or uint32
        return JS_VALUE_GET_TAG(val) == JS_TAG_INT;
    }

    bool Data::IsNumber() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsNumber(val);
    }

    bool Data::IsExternal() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_VALUE_GET_TAG(val) == jsb::impl::JS_TAG_EXTERNAL;
    }

    bool Data::IsBoolean() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsBool(val);
    }

    bool Data::IsBigInt() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsBigInt(isolate_->ctx(), val);
    }

    bool Data::IsArrayBuffer() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        return JS_IsArrayBuffer(val);
    }

    bool Data::strict_eq(const Data& other) const
    {
        const JSValue val1 = isolate_->stack_val(stack_pos_);
        const JSValue val2 = isolate_->stack_val(other.stack_pos_);
        return jsb::impl::QuickJS::Equals(val1, val2);
    }

}
