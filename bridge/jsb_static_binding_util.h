﻿#ifndef GODOTJS_STATIC_BINDING_UTIL_H
#define GODOTJS_STATIC_BINDING_UTIL_H
#include "jsb_bridge_pch.h"

#include "jsb_type_convert.h"

#define DEF_VARIANT_THIS_UTIL(Type, ReaderFunc) \
    template<>\
    struct PrimitiveInstanceUtil<Type> {\
        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Object>& p_input, Type*& r_value) {\
            if (!TypeConvert::is_variant(p_input)) return false;\
            r_value = VariantInternal::ReaderFunc((Variant*) p_input->GetAlignedPointerFromInternalField(IF_Pointer));\
            return true;\
        }\
    }

// #define DEF_VARIANT_THIS_UTIL_CONST(Type, ReaderFunc) \
//     struct PrimitiveInstanceUtil<const Type> {\
//         static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Object>& p_input, Type const*& r_value) {\
//             r_value = VariantInternal::ReaderFunc((const Variant*) p_input->GetAlignedPointerFromInternalField(IF_Pointer));\
//             return true;\
//         }\
//     }

namespace jsb
{
    template<typename T>
    struct PrimitiveInstanceUtil
    {
        // no implementation
        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Object>& p_input, T*& r_value);
    };

    // Atomic types.
    // omitted bool, get_bool
    // omitted int64_t, get_int
    // omitted double, get_float
    // omitted String, get_string

    // Math types.
    DEF_VARIANT_THIS_UTIL(Vector2, get_vector2);
    DEF_VARIANT_THIS_UTIL(Vector2i, get_vector2i);
    DEF_VARIANT_THIS_UTIL(Rect2, get_rect2);
    DEF_VARIANT_THIS_UTIL(Rect2i, get_rect2i);
    DEF_VARIANT_THIS_UTIL(Vector3, get_vector3);
    DEF_VARIANT_THIS_UTIL(Vector3i, get_vector3i);
    DEF_VARIANT_THIS_UTIL(Vector4, get_vector4);
    DEF_VARIANT_THIS_UTIL(Vector4i, get_vector4i);
    DEF_VARIANT_THIS_UTIL(Transform2D, get_transform2d);
    DEF_VARIANT_THIS_UTIL(Plane, get_plane);
    DEF_VARIANT_THIS_UTIL(Quaternion, get_quaternion);
    DEF_VARIANT_THIS_UTIL(::AABB, get_aabb);
    DEF_VARIANT_THIS_UTIL(::Basis, get_basis);
    DEF_VARIANT_THIS_UTIL(Transform3D, get_transform);
    DEF_VARIANT_THIS_UTIL(Projection, get_projection);

    // Misc types.
    DEF_VARIANT_THIS_UTIL(Color, get_color);
    // omitted StringName
    DEF_VARIANT_THIS_UTIL(NodePath, get_node_path);
    DEF_VARIANT_THIS_UTIL(RID, get_rid);
    DEF_VARIANT_THIS_UTIL(Callable, get_callable);
    DEF_VARIANT_THIS_UTIL(Signal, get_signal);
    DEF_VARIANT_THIS_UTIL(Dictionary, get_dictionary);
    DEF_VARIANT_THIS_UTIL(Array, get_array);

    // Typed arrays.
    DEF_VARIANT_THIS_UTIL(PackedByteArray, get_byte_array);
    DEF_VARIANT_THIS_UTIL(PackedInt32Array, get_int32_array);
    DEF_VARIANT_THIS_UTIL(PackedInt64Array, get_int64_array);
    DEF_VARIANT_THIS_UTIL(PackedFloat32Array, get_float32_array);
    DEF_VARIANT_THIS_UTIL(PackedFloat64Array, get_float64_array);
    DEF_VARIANT_THIS_UTIL(PackedStringArray, get_string_array);
    DEF_VARIANT_THIS_UTIL(PackedVector2Array, get_vector2_array);
    DEF_VARIANT_THIS_UTIL(PackedVector3Array, get_vector3_array);
    DEF_VARIANT_THIS_UTIL(PackedColorArray, get_color_array);
#if GODOT_VERSION_NEWER_THAN(4, 3, 0)
    DEF_VARIANT_THIS_UTIL(PackedVector4Array, get_vector4_array);
#endif

    // fallback to Variant transpiler
    template<typename T>
    struct StaticBindingUtil
    {
        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_input, T& r_value)
        {
            Variant cv;
            if (TypeConvert::js_to_gd_var(isolate, context, p_input, GetTypeInfo<T>::VARIANT_TYPE, cv))
            {
                jsb_check(cv.get_type() == GetTypeInfo<T>::VARIANT_TYPE);
                r_value = cv;
                return true;
            }
            return false;
        }

        static bool set(v8::Isolate* isolate, const v8::Local<v8::Context>& context, T const& p_input, v8::Local<v8::Value>& r_value)
        {
            return TypeConvert::gd_var_to_js(isolate, context, (Variant) p_input, r_value);
        }
    };

    template<>
    struct StaticBindingUtil<Object*>
    {
        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_input, Object*& r_value)
        {
            return TypeConvert::js_to_gd_obj(isolate, context, p_input, r_value);
        }

        static bool set(v8::Isolate* isolate, const v8::Local<v8::Context>& context, Object* const& p_input, v8::Local<v8::Value>& r_value)
        {
            v8::Local<v8::Object> obj;
            if (TypeConvert::gd_obj_to_js(isolate, context, p_input, obj))
            {
                r_value = obj;
                return true;
            }
            return false;
        }
    };

    template<>
    struct StaticBindingUtil<float>
    {
        static bool get(const v8::Local<v8::Value>& p_input, float& r_value)
        {
            if (p_input->IsNumber())
            {
                r_value = (float) p_input.As<v8::Number>()->Value();
                return true;
            }
            return false;
        }

        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_input, float& r_value)
        {
            if (p_input->IsNumber())
            {
                r_value = (float) p_input.As<v8::Number>()->Value();
                return true;
            }
            return false;
        }

        static bool set(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const float& p_input, v8::Local<v8::Value>& r_value)
        {
            r_value = v8::Number::New(isolate, (float) p_input);
            return true;
        }
    };

    template<>
    struct StaticBindingUtil<double>
    {
        static bool get(const v8::Local<v8::Value>& p_input, double& r_value)
        {
            if (p_input->IsNumber())
            {
                r_value = (double) p_input.As<v8::Number>()->Value();
                return true;
            }
            return false;
        }

        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_input, double& r_value)
        {
            if (p_input->IsNumber())
            {
                r_value = (double) p_input.As<v8::Number>()->Value();
                return true;
            }
            return false;
        }

        static bool set(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const double& p_input, v8::Local<v8::Value>& r_value)
        {
            r_value = v8::Number::New(isolate, (double) p_input);
            return true;
        }
    };

    template<>
    struct StaticBindingUtil<int64_t>
    {
        // for hardcoded call
        static bool get(const v8::Local<v8::Value>& p_input, int64_t& r_value)
        {
            return impl::Helper::to_int64(p_input, r_value);
        }

        // for template-based call
        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_input, int64_t& r_value)
        {
            return impl::Helper::to_int64(p_input, r_value);
        }

        static bool set(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const int64_t& p_input, v8::Local<v8::Value>& r_value)
        {
            r_value = impl::Helper::new_integer(isolate, p_input);
            return true;
        }
    };

    template<>
    struct StaticBindingUtil<int32_t>
    {
        static bool get(const v8::Local<v8::Value>& p_input, int32_t& r_value)
        {
            if (p_input->IsNumber())
            {
                r_value = (int32_t) p_input.As<v8::Int32>()->Value();
                return true;
            }
            return false;
        }

        static bool get(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_input, int32_t& r_value)
        {
            if (p_input->IsNumber())
            {
                r_value = (int32_t) p_input.As<v8::Int32>()->Value();
                return true;
            }
            return false;
        }

        static bool set(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const int32_t& p_input, v8::Local<v8::Value>& r_value)
        {
            r_value = impl::Helper::new_integer(isolate, p_input);
            return true;
        }
    };
}

#endif
