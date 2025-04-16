#include "jsb_quickjs_object.h"
#include "jsb_quickjs_isolate.h"
#include "jsb_quickjs_context.h"
#include "jsb_quickjs_function_interop.h"

namespace v8
{
    int Object::InternalFieldCount() const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        const jsb::impl::InternalDataID index = (jsb::impl::InternalDataID)(uintptr_t) JS_GetOpaque(val, isolate_->get_class_id());
        if (!index) return 0;
        const jsb::impl::InternalDataPtr data = isolate_->get_internal_data(index);
        return data->internal_field_count;
    }

    void Object::SetAlignedPointerInInternalField(int slot, void* data)
    {
        jsb_check((uintptr_t) data % 2 == 0);
        const JSValue val = isolate_->stack_val(stack_pos_);
        const jsb::impl::InternalDataID internal_data_id = (jsb::impl::InternalDataID)(uintptr_t) JS_GetOpaque(val, isolate_->get_class_id());
        const jsb::impl::InternalDataPtr internal_data = isolate_->get_internal_data(internal_data_id);
        JSB_QUICKJS_LOG(VeryVerbose, "set internal data JSObject:%s id:%s data:%s (last:%s)", (uintptr_t) JS_VALUE_GET_PTR(val), internal_data_id, (uintptr_t) data, (uintptr_t) internal_data->internal_fields[slot]);
        jsb_checkf(!data || !internal_data->internal_fields[slot], "overwriting the internal field is not allowed");
        internal_data->internal_fields[slot] = data;
    }

    void Object::SetAlignedPointerInInternalFields(int argc, int indices[], void* values[])
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        const jsb::impl::InternalDataID internal_data_id = (jsb::impl::InternalDataID)(uintptr_t) JS_GetOpaque(val, isolate_->get_class_id());
        const jsb::impl::InternalDataPtr internal_data = isolate_->get_internal_data(internal_data_id);
        for (int i = 0; i < argc; i++)
        {
            jsb_check((uintptr_t) values[i] % 2 == 0);
            jsb_checkf(!values[i] || !internal_data->internal_fields[indices[i]], "overwriting the internal field #%d is not allowed", indices[i]);
            internal_data->internal_fields[indices[i]] = values[i];
        }
    }

    void* Object::GetAlignedPointerFromInternalField(int slot) const
    {
        const JSValue val = isolate_->stack_val(stack_pos_);
        const jsb::impl::InternalDataID index = (jsb::impl::InternalDataID)(uintptr_t) JS_GetOpaque(val, isolate_->get_class_id());
        const jsb::impl::InternalDataPtr data = isolate_->get_internal_data(index);
        return data->internal_fields[slot];
    }

    Local<String> Object::GetConstructorName()
    {
        const JSValue self = isolate_->stack_val(stack_pos_);
        jsb_check(JS_IsFunction(isolate_->ctx(), self) || JS_IsObject(self));

        const JSValue name = JS_GetProperty(isolate_->ctx(), self, jsb::impl::JS_ATOM_name);
        if (JS_IsException(name))
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(isolate_->ctx());
            return Local<String>();
        }
        if (!JS_IsString(name))
        {
            JS_FreeValue(isolate_->ctx(), name);
            return Local<String>();
        }
        return Local<String>(Data(isolate_, isolate_->push_steal(name)));
    }

    Maybe<bool> Object::Set(Local<Context> context, uint32_t index, Local<Value> value)
    {
        const JSValue self = isolate_->stack_val(stack_pos_);
        jsb_check(JS_IsArray(isolate_->ctx(), self));
        const int res = JS_SetPropertyUint32(isolate_->ctx(), self, index, JS_DupValue(isolate_->ctx(), (JSValue) value));
        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(isolate_->ctx());
            return Maybe<bool>();
        }
        return Maybe<bool>(true);
    }

    MaybeLocal<Value> Object::Get(Local<Context> context, uint32_t index) const
    {
        const JSValue self = isolate_->stack_val(stack_pos_);
        jsb_check(JS_IsArray(isolate_->ctx(), self));
        const JSValue val = JS_GetPropertyUint32(isolate_->ctx(), self, index);
        if (JS_IsException(val))
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(isolate_->ctx());
            return MaybeLocal<Value>();
        }
        return MaybeLocal<Value>(Data(isolate_, isolate_->push_steal(val)));
    }

    Maybe<bool> Object::Set(Local<Context> context, Local<Value> key, Local<Value> value)
    {
        const JSValue self = isolate_->stack_val(stack_pos_);
        JSContext* ctx = isolate_->ctx();

        const jsb::impl::QuickJS::Atom index(ctx, (JSValue) key);
        const int res = JS_SetProperty(ctx, self, index, JS_DupValue(ctx, (JSValue) value));
        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(isolate_->ctx());
            return Maybe<bool>();
        }
        return Maybe<bool>(true);
    }

    MaybeLocal<Value> Object::Get(Local<Context> context, Local<Value> key) const
    {
        const JSValue self = isolate_->stack_val(stack_pos_);
        JSContext* ctx = isolate_->ctx();

        const jsb::impl::QuickJS::Atom index(ctx, (JSValue) key);
        const JSValue val = JS_GetProperty(ctx, self, index);
        if (JS_IsException(val))
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return MaybeLocal<Value>();
        }
        return MaybeLocal<Value>(Data(isolate_, isolate_->push_steal(val)));
    }

    Maybe<bool> Object::HasOwnProperty(Local<Context> context, Local<Name> key) const
    {
        //TODO unsure
        JSContext* ctx = isolate_->ctx();
        const jsb::impl::QuickJS::Atom prop(ctx, (JSValue) key);
        const int res = JS_GetOwnProperty(ctx, nullptr, (JSValue) *this, prop);

        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(isolate_->ctx());
            return Maybe<bool>();
        }
        return Maybe<bool>(!!res);
    }

    Maybe<bool> Object::SetPrototype(Local<Context> context, Local<Value> prototype)
    {
        JSContext* ctx = isolate_->ctx();
        const int res = JS_SetPrototype(ctx, (JSValue) *this, (JSValue) prototype);
        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return Maybe<bool>();
        }
        return Maybe<bool>(!!res);
    }

    Local<Value> Object::GetPrototype()
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue prototype = JS_GetPrototype(ctx, (JSValue) *this);
        if (JS_IsException(prototype))
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return Local<Value>();
        }
        return Local<Value>(Data(isolate_, isolate_->push_steal(prototype)));
    }

    MaybeLocal<Value> Object::CallAsConstructor(Local<Context> context, int argc, Local<Value> argv[])
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue self = (JSValue) *this;
        JSValue* argvv = jsb_stackalloc(JSValue, argc);
        for (int i = 0; i < argc; ++i)
        {
            argvv[i] = (JSValue) argv[i];
        }
        const JSValue instance = JS_CallConstructor(ctx, self, argc, argvv);
        if (JS_IsException(instance))
        {
            // intentionally keep the exception
            return MaybeLocal<Value>();
        }
        return MaybeLocal<Value>(Data(isolate_, isolate_->push_steal(instance)));
    }

    Local<Object> Object::New(Isolate* isolate)
    {
        JSContext* ctx = isolate->ctx();
        const JSValue obj = JS_NewObject(ctx);
        jsb_check(!JS_IsException(obj));
        return Local<Object>(Data(isolate, isolate->push_steal(obj)));
    }

    MaybeLocal<Value> Object::GetOwnPropertyDescriptor(Local<Context> context, Local<Name> key) const
    {
        JSContext* ctx = isolate_->ctx();
        JSPropertyDescriptor desc;

        const JSValue self = (JSValue) *this;
        const jsb::impl::QuickJS::Atom prop(ctx, (JSValue) key);
        const int res = JS_GetOwnProperty(ctx, &desc, self, prop);

        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return MaybeLocal<Value>();
        }
        // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Object/getOwnPropertyDescriptor
        const JSValue desc_js = JS_NewObject(ctx);
        jsb_check(JS_IsObject(desc_js));

        // JSValues in desc are free-ed by set
        jsb_ensure(JS_SetProperty(ctx, desc_js, jsb::impl::JS_ATOM_get, desc.getter) != -1);
        jsb_ensure(JS_SetProperty(ctx, desc_js, jsb::impl::JS_ATOM_set, desc.setter) != -1);
        jsb_ensure(JS_SetProperty(ctx, desc_js, jsb::impl::JS_ATOM_value, desc.value) != -1);

        // property flags
        jsb_ensure(JS_SetProperty(ctx, desc_js, jsb::impl::JS_ATOM_configurable, JS_MKVAL(JS_TAG_BOOL, !!(desc.flags & JS_PROP_CONFIGURABLE))) != -1);
        jsb_ensure(JS_SetProperty(ctx, desc_js, jsb::impl::JS_ATOM_writable, JS_MKVAL(JS_TAG_BOOL, !!(desc.flags & JS_PROP_WRITABLE))) != -1);
        jsb_ensure(JS_SetProperty(ctx, desc_js, jsb::impl::JS_ATOM_enumerable, JS_MKVAL(JS_TAG_BOOL, !!(desc.flags & JS_PROP_ENUMERABLE))) != -1);

        return MaybeLocal<Value>(Data(isolate_, isolate_->push_steal(desc_js)));
    }

    JSValue Object::_lazy(JSContext* ctx, JSValue this_val, int argc, JSValue* argv, int magic, JSValue* func_data)
    {
        Isolate* isolate = (Isolate*) JS_GetContextOpaque(ctx);
        JSValue rvo;

        // evaluate lazy callback
        {
            const AccessorNameGetterCallback getter = (AccessorNameGetterCallback) JS_VALUE_GET_PTR(func_data[1]);
            HandleScope handle_scope(isolate);

            const uint16_t rvo_pos = isolate->push_copy(JS_UNDEFINED); // return value
            const PropertyCallbackInfo<Value> info(isolate, rvo_pos);
            const Local<Name> prop_v(Data(isolate, isolate->push_copy(func_data[0])));

            getter(prop_v, info);

            // after back from the impl layer, we need to return JS_EXCEPTION if an error is thrown in quickjs
            if (isolate->is_error_thrown())
            {
                return JS_EXCEPTION;
            }

            // We need a duplicated value for returning.
            // Duplicate the value here because the stack value will become invalid after leaving the handle scope.
            rvo = isolate->stack_dup(rvo_pos);
            jsb_check(!JS_IsException(rvo));
        }

        // overwrite the current lazy getter with rvo
        {
            const jsb::impl::QuickJS::Atom prop(ctx, func_data[0]);
            constexpr int flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_VALUE;

            //NOTE !!! JS_DefineProperty DOES NOT CONSUME THE REFERENCE !!!
            const int res = JS_DefineProperty(ctx, this_val, prop, rvo, JS_UNDEFINED, JS_UNDEFINED, flags);
            jsb_check(res >= 0);
            jsb_unused(res);
        }

        return rvo;
    }

    Maybe<bool> Object::SetLazyDataProperty(Local<Context> context, Local<Name> name, AccessorNameGetterCallback getter)
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue this_obj = (JSValue) *this;
        constexpr int flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_GET;
        JSValue lazy_data[] = { JS_DupValue(ctx, (JSValue) name), JS_MKPTR(jsb::impl::JS_TAG_EXTERNAL, (void *) getter) };
        const JSValue lazy = JS_NewCFunctionData(ctx, _lazy, /* length */ 0, /* magic */ 0, ::std::size(lazy_data), lazy_data);

        const jsb::impl::QuickJS::Atom prop(ctx, (JSValue) name);
        const int res = JS_DefineProperty(ctx, this_obj, prop, JS_UNDEFINED, lazy, JS_UNDEFINED, flags);

        //NOTE !!! JS_DefineProperty DOES NOT CONSUME THE REFERENCE !!!
        JS_FreeValue(ctx, lazy);

        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return Maybe<bool>();
        }
        return Maybe<bool>(!!res);
    }

    Maybe<bool> Object::DefineOwnProperty(Local<Context> context, Local<Name> key, Local<Value> value, PropertyAttribute attributes)
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue this_obj = (JSValue) *this;
        const JSValue val = (JSValue) value;
        int flags = JS_PROP_HAS_VALUE;
        if ((attributes & DontEnum) == 0) flags |= JS_PROP_HAS_ENUMERABLE;
        if ((attributes & ReadOnly) == 0) flags |= JS_PROP_HAS_WRITABLE;
        if ((attributes & DontDelete) == 0) flags |= JS_PROP_HAS_CONFIGURABLE;

        const jsb::impl::QuickJS::Atom prop(ctx, (JSValue) key);
        //NOTE !!! JS_DefineProperty DOES NOT CONSUME THE REFERENCE !!!
        const int res = JS_DefineProperty(ctx, this_obj, prop, val, JS_UNDEFINED, JS_UNDEFINED, flags);

        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return Maybe<bool>();
        }
        return Maybe<bool>(!!res);
    }

    void Object::SetAccessorProperty(Local<Name> name, Local<FunctionTemplate> getter, Local<FunctionTemplate> setter)
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue this_obj = (JSValue) *this;
        int flags = JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE; //TODO consider remove 'configurable' flags
        if (!getter.IsEmpty()) flags |= JS_PROP_HAS_GET;
        if (!setter.IsEmpty()) flags |= JS_PROP_HAS_SET | JS_PROP_HAS_WRITABLE;

        const jsb::impl::QuickJS::Atom prop(ctx, (JSValue) name);
        const int res = JS_DefineProperty(ctx, this_obj, prop,
            JS_UNDEFINED,
            (JSValue) getter, //NOTE !!! JS_DefineProperty DOES NOT CONSUME THE REFERENCE !!!
            (JSValue) setter,
            flags);
        jsb_check(res >= 0);
        jsb_unused(res);
    }

    MaybeLocal<Array> Object::GetOwnPropertyNames(Local<Context> context, PropertyFilter filter, KeyConversionMode key_conversion)
    {
        JSContext* ctx = isolate_->ctx();
        JSPropertyEnum *tab;
        uint32_t len;

        int flags = 0;
        if ((filter & SKIP_STRINGS) == 0) flags |= JS_GPN_STRING_MASK;
        if ((filter & SKIP_SYMBOLS) == 0) flags |= JS_GPN_SYMBOL_MASK;

        // key_conversion is not available in quickjs.impl
        jsb_check(key_conversion == v8::KeyConversionMode::kNoNumbers);

        int res = JS_GetOwnPropertyNames(ctx, &tab, &len, (JSValue) *this, flags);
        if (res == -1)
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return MaybeLocal<Array>();
        }

        // build the name table
        const JSValue array = JS_NewArray(ctx);

        for(uint32_t i = 0; i < len; ++i)
        {
            res = JS_SetPropertyUint32(ctx, array, i, JS_AtomToValue(ctx, tab[i].atom));
            jsb_check(res >= 0);

            // cleanup
            JS_FreeAtom(ctx, tab[i].atom);
        }
        js_free(ctx, tab);

        return MaybeLocal<Array>(Data(isolate_, isolate_->push_steal(array)));
    }

    MaybeLocal<Promise::Resolver> Promise::Resolver::New(Local<Context> context)
    {
        Isolate* isolate = context->isolate_;
        JSContext* ctx = isolate->ctx();
        JSValue resolvers[] = { JS_UNDEFINED, JS_UNDEFINED };
        const JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
        if (JS_IsException(promise))
        {
            jsb::impl::QuickJS::MarkExceptionAsTrivial(ctx);
            return MaybeLocal<Promise::Resolver>();
        }
        
        // need an object to hold all three elements at the same time to represent it as a Resolver.
        // we don't want to break the simplicity of the implementations of `Data` class and `Global` handle.
        // so use an indirect array to hold all of them. 
        const JSValue holder = JS_NewArray(ctx);
        jsb_ensure(!JS_IsException(holder));
        jsb_check(JS_IsArray(ctx, holder));

        // these references are consumed by SetProperty, no additional FreeValue is needed
        JS_SetPropertyUint32(ctx, holder, kHolderIndexResolve, resolvers[0]); // resolve
        JS_SetPropertyUint32(ctx, holder, kHolderIndexReject,  resolvers[1]); // reject
        JS_SetPropertyUint32(ctx, holder, kHolderIndexPromise, promise); 
        
        return MaybeLocal<Promise::Resolver>(Data(isolate, isolate->push_steal(holder)));
    }

    namespace
    {
        uint32_t get_packed_array_length(JSContext* ctx, JSValueConst val)
        {
            const JSValue pv = JS_GetProperty(ctx, val, jsb::impl::JS_ATOM_length);
            jsb_check(!JS_IsException(pv));
            uint32_t length;
            const int failed = JS_ToUint32(ctx, &length, pv);
            JS_FreeValue(ctx, pv);
            jsb_check(failed == 0);
            jsb_unused(failed);
            return length;
        }
    }

    Local<Promise> Promise::Resolver::GetPromise()
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue holder = (JSValue) *this;
        jsb_check(JS_IsArray(ctx, holder));
        jsb_check(get_packed_array_length(ctx, holder) == kHolderIndexCount);
        
        const JSValue obj = JS_GetPropertyUint32(ctx, holder, kHolderIndexPromise);
        jsb_check(JS_IsPromise(obj));
        return Local<Promise>(Data(isolate_, isolate_->push_steal(obj)));
    }
            
    Maybe<bool> Promise::Resolver::Resolve(Local<Context> context, Local<Value> value)
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue holder = (JSValue) *this;
        jsb_check(JS_IsArray(ctx, holder));
        jsb_check(get_packed_array_length(ctx, holder) == kHolderIndexCount);

        // unpack the resolve function 
        const JSValue obj = JS_GetPropertyUint32(ctx, holder, kHolderIndexResolve);
        jsb_check(JS_IsFunction(ctx, obj));

        // call the resolve function
        JSValue args[] = { (JSValue) value };
        const JSValue rval = JS_Call(ctx, obj, JS_UNDEFINED, std::size(args), args);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(rval))
        {
            // leave the exception as it is
            return Maybe<bool>();
        }
        JS_FreeValue(ctx, rval);
        return Maybe<bool>(true);
    }

    Maybe<bool> Promise::Resolver::Reject(Local<Context> context, Local<Value> value)
    {
        JSContext* ctx = isolate_->ctx();
        const JSValue holder = (JSValue) *this;
        jsb_check(JS_IsArray(ctx, holder));
        jsb_check(get_packed_array_length(ctx, holder) == kHolderIndexCount);

        // unpack the reject function 
        const JSValue obj = JS_GetPropertyUint32(ctx, holder, kHolderIndexReject);
        jsb_check(JS_IsFunction(ctx, obj));
        
        // call the reject function
        JSValue args[] = { (JSValue) value };
        const JSValue rval = JS_Call(ctx, obj, JS_UNDEFINED, std::size(args), args);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(rval))
        {
            // leave the exception as it is
            return Maybe<bool>();
        }
        JS_FreeValue(ctx, rval);
        return Maybe<bool>(true);
    }
}
