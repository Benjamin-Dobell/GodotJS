﻿#ifndef GODOTJS_TYPE_CONVERT_H
#define GODOTJS_TYPE_CONVERT_H

#include "jsb_bridge_pch.h"
#include "jsb_object_handle.h"
#include "jsb_class_info.h"

namespace jsb
{
    struct TypeConvert
    {
        /**
         * Returns a string representation of a JavaScript type. For primitives, equivalent to the typeof operator in
         * JS. For JS objects, we perform additional inspections and display if the object is an array, the Godot
         * variant type, or the constructor name, when available.
         */
        static String js_debug_typeof(v8::Isolate* isolate, const v8::Local<v8::Value>& p_jval);

        /**
         * Translate a Godot object into a javascript object. The type of `p_object_obj` will be automatically exposed to the context if not existed.
         * @param p_godot_obj non-null godot object pointer
         */
        static bool gd_obj_to_js(v8::Isolate* isolate, const v8::Local<v8::Context>& context, Object* p_godot_obj, v8::Local<v8::Object>& r_jval);

        /**
         * return false if strict type check fails.
         * dead objects return true with a nullptr.
         * NOTE: ensure p_jval is not empty before calling this function.
         */
        static bool js_to_gd_obj(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_jval, Object*& r_godot_obj);

        jsb_force_inline static bool gd_var_to_js(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const Variant& p_cvar, v8::Local<v8::Value>& r_jval) { return gd_var_to_js(isolate, context, p_cvar, p_cvar.get_type(), r_jval); }
        static bool gd_var_to_js(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const Variant& p_cvar, Variant::Type p_type, v8::Local<v8::Value>& r_jval);
        static bool js_to_gd_var(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_jval, Variant::Type p_type, Variant& r_cvar);

        /**
         * Translate js val into gd variant without any type hint
         */
        static bool js_to_gd_var(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_jval, Variant& r_cvar);

        /**
         * Check if a javascript value `p_val` could be converted into the expected primitive type `p_type`
         */
        static bool can_convert_strict(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_val, Variant::Type p_type);

        // variant fast check (without checking NativeClassInfo)
        jsb_force_inline static bool is_variant(const v8::Local<v8::Object>& p_obj)
        {
            return p_obj->InternalFieldCount() == IF_VariantFieldCount;
        }

        // object fast check (without checking NativeClassInfo)
        jsb_force_inline static bool is_object(const v8::Local<v8::Object>& p_obj)
        {
            return p_obj->InternalFieldCount() == IF_ObjectFieldCount;
        }

        // object fast check (without checking NativeClassInfo)
        jsb_force_inline static bool is_object(const v8::Local<v8::Object>& p_obj, NativeClassType::Type p_type)
        {
            if (!is_object(p_obj)) return false;
            return (NativeClassType::Type)(uintptr_t) p_obj->GetAlignedPointerFromInternalField(IF_ClassType) == p_type;
        }

    };
}

#endif
