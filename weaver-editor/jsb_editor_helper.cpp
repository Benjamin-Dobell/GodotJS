#include "jsb_editor_helper.h"

#include "../bridge/jsb_bridge_helper.h"
#include "../bridge/jsb_callable.h"
#include "../bridge/jsb_type_convert.h"
#include "../weaver/jsb_script.h"

#include "editor/gui/editor_toaster.h"
#include "scene/animation/animation_mixer.h"
#include "scene/resources/packed_scene.h"

// The following enums must be kept in sync with jsb.editor.codegen.ts
enum class CodeGenType {
    ScriptNodeTypeDescriptor,
    ScriptResourceTypeDescriptor,
};
enum class DescriptorType {
    Godot,
    User,
    FunctionLiteral,
    ObjectLiteral,
    StringLiteral,
    NumericLiteral,
    BooleanLiteral,
    Union,
    Intersection,
    Conditional,
    Tuple,
    Infer,
    Mapped,
    Indexed,
};

bool GodotJSEditorHelper::_request_codegen(jsb::JSEnvironment& p_env, GodotJSScript* p_script, const Dictionary& p_request, Dictionary& p_result)
{
    v8::Isolate* isolate = p_env->get_isolate();
    v8::Local<v8::Context> context = p_env->get_context();

    p_script->load_module_if_missing();
    String module_id = p_script->get_module_id();
    jsb::JavaScriptModule* module = p_env->get_module_cache().find(module_id);

    if (module == nullptr)
    {
        JSB_LOG(Warning, "Codegen failed to load module script '%s'.", p_script->get_path());
        return false;
    }

    if (module->exports.IsEmpty())
    {
        return false;
    }

    v8::Local<v8::Value> module_exports = module->exports.Get(isolate);

    if (module_exports.IsEmpty() || !module_exports->IsObject())
    {
        return false;
    }

    const v8::Local<v8::Value> codegen_func_val = module_exports.As<v8::Object>()->Get(context, jsb_name(p_env, codegen)).ToLocalChecked();

    if (codegen_func_val.IsEmpty() || !codegen_func_val->IsFunction())
    {
        return false;
    }

    const v8::Local<v8::Function> codegen_func = codegen_func_val.As<v8::Function>();

    v8::Local<v8::Value> codegen_request_val;

    if (!jsb::TypeConvert::gd_var_to_js(isolate, context, p_request, codegen_request_val))
    {
        JSB_LOG(Error, "Codegen failed for module '%s': Failed to convert codegen request", module_id);
        return false;
    }

    jsb::impl::TryCatch try_catch(isolate);

    v8::Local<v8::Value> argv[] = { codegen_request_val };
    const v8::MaybeLocal<v8::Value> maybe_result = codegen_func->Call(context, v8::Undefined(isolate), std::size(argv), argv);

    if (try_catch.has_caught())
    {
        JSB_LOG(Error, "Codegen failed for module '%s': %s", module_id, jsb::BridgeHelper::get_exception(try_catch));
        return false;
    }

    v8::Local<v8::Value> result_val;
    Variant result;

    if (!maybe_result.ToLocal(&result_val) || !jsb::TypeConvert::js_to_gd_var(isolate, context, result_val, Variant::Type::DICTIONARY, result))
    {
        return false;
    }

    p_result = result;
    return true;
}

StringName GodotJSEditorHelper::_get_exposed_node_class_name(const StringName& class_name)
{
    StringName exposed_class_name = class_name;

    while (!jsb::internal::NamingUtil::is_original_class_exposed(exposed_class_name))
    {
        exposed_class_name = ClassDB::get_parent_class(exposed_class_name);
    }

    return jsb::internal::NamingUtil::get_class_name(exposed_class_name);
}

Dictionary GodotJSEditorHelper::_build_node_type_descriptor(jsb::JSEnvironment& p_env, Node* p_node, const String& p_scene_resource_path)
{
    Dictionary descriptor;
    Dictionary children;
    int child_count = p_node->get_child_count(true);

    for (int i = 0; i < child_count; i++)
    {
        Node* child = p_node->get_child(i, true);
        children[child->get_name()] = _build_node_type_descriptor(p_env, child);
    }

    ScriptInstance* script_instance = p_node->get_script_instance();
    GodotJSScript* script = script_instance != nullptr ? Object::cast_to<GodotJSScript>(*script_instance->get_script()) : nullptr;

    if (script != nullptr)
    {
        Dictionary codegen_request;
        codegen_request[jsb_string_name(type)] = (int32_t) CodeGenType::ScriptNodeTypeDescriptor;
        codegen_request[jsb_string_name(node)] = p_node;
        codegen_request[jsb_string_name(children)] = children;

        if (!_request_codegen(p_env, script, codegen_request, descriptor))
        {
            descriptor.clear();
        }
    }

    if (descriptor.is_empty())
    {
        // By default, only scene (and sub-scene) roots are typed with a user defined type. This ensures that classes are
        // able to use SceneNodes in their type declaration without illegally referencing their own type. Users can use
        // codegen to override this behavior.
        if (script == nullptr
            || p_node->get_scene_file_path().is_empty()
            || GodotJSScriptLanguage::get_singleton()->is_global_class_generic(script->get_path())
            || script->get_global_name().is_empty())
        {
            Dictionary object_literal;
            object_literal[jsb_string_name(type)] = (int32_t) DescriptorType::ObjectLiteral;
            object_literal[jsb_string_name(properties)] = children;

            Array generic_arguments;
            generic_arguments.push_back(object_literal);

            AnimationMixer *animation_mixer = Object::cast_to<AnimationMixer>(p_node);

            if (animation_mixer)
            {
#if GODOT_4_7_OR_NEWER
                LocalVector<StringName> library_names;
#else
                List<StringName> library_names;
#endif

                animation_mixer->get_animation_library_list(&library_names);

                Dictionary animation_libraries_object_literal;
                Dictionary animation_libraries_properties;
                animation_libraries_object_literal[jsb_string_name(type)] = (int32_t) DescriptorType::ObjectLiteral;
                animation_libraries_object_literal[jsb_string_name(properties)] = animation_libraries_properties;

                for (const StringName &library_name : library_names)
                {
                    Ref<AnimationLibrary> library = animation_mixer->get_animation_library(library_name);

                    Array animation_names_union_array;

#if GODOT_4_7_OR_NEWER
                    LocalVector<StringName> animation_names;
#else
                    List<StringName> animation_names;
#endif

                    library->get_animation_list(&animation_names);

                    for (const StringName& animation_name : animation_names)
                    {
                        Dictionary string_literal;
                        string_literal[jsb_string_name(type)] = (int32_t) DescriptorType::StringLiteral;
                        string_literal[jsb_string_name(value)] = animation_name;
                        animation_names_union_array.push_back(string_literal);
                    }

                    Dictionary animation_names_union;
                    animation_names_union[jsb_string_name(type)] = (int32_t) DescriptorType::Union;
                    animation_names_union["types"] = animation_names_union_array;

                    Array animation_generic_arguments;
                    animation_generic_arguments.push_back(animation_names_union);

                    Dictionary animation_library_descriptor;
                    animation_library_descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::Godot;
                    animation_library_descriptor[jsb_string_name(name)] = jsb::internal::NamingUtil::get_class_name("AnimationLibrary");
                    animation_library_descriptor[jsb_string_name(arguments)] = animation_generic_arguments;

                    animation_libraries_properties[library_name] = animation_library_descriptor;
                }

                generic_arguments.push_back(animation_libraries_object_literal);
            }

            descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::Godot;
            descriptor[jsb_string_name(name)] = GodotJSEditorHelper::_get_exposed_node_class_name(p_node->get_class_name());
            descriptor[jsb_string_name(arguments)] = generic_arguments;
        }
        else
        {
            descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::User;
            descriptor[jsb_string_name(name)] = script->get_global_name();
            descriptor[jsb_string_name(resource)] = script->get_path();
        }
    }

    // Optionally replace children literal with SceneNodes["path/to/scene.tscn"]
    if (!p_scene_resource_path.is_empty())
    {
        Variant arguments_var = descriptor[jsb_string_name(arguments)];

        if (arguments_var.is_array())
        {
            Array arguments = arguments_var;
            int argument_count = arguments.size();

            Dictionary children_descriptor;
            children_descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::ObjectLiteral;
            children_descriptor[jsb_string_name(properties)] = children;

            for (int i = 0; i < argument_count; i++)
            {
                Variant argument = arguments[i];

                if (argument.get_type() == Variant::Type::DICTIONARY && Dictionary(argument) == children_descriptor)
                {
                    Dictionary scene_nodes;
                    scene_nodes[jsb_string_name(type)] = (int32_t) DescriptorType::Godot;
                    scene_nodes[jsb_string_name(name)] = "SceneNodes";

                    Dictionary string_literal;
                    string_literal[jsb_string_name(type)] = (int32_t) DescriptorType::StringLiteral;
                    string_literal[jsb_string_name(value)] = p_scene_resource_path.substr(6); // Remove leading res://

                    Dictionary indexed_scene_nodes;
                    indexed_scene_nodes[jsb_string_name(type)] = (int32_t) DescriptorType::Indexed;
                    indexed_scene_nodes[jsb_string_name(base)] = scene_nodes;
                    indexed_scene_nodes[jsb_string_name(index)] = string_literal;

                    arguments[i] = indexed_scene_nodes;
                }
            }
        }
    }

    return descriptor;
}

// Similar logic to EditorNode::_dialog_display_load_error.
void GodotJSEditorHelper::_log_load_error(const String &p_file, const String &p_type, Error p_error)
{
    if (p_error)
    {
        switch (p_error)
        {
            case ERR_CANT_OPEN:
            {
                JSB_LOG(Error, "Can't open file '%s'. The file could have been moved or deleted.", p_file.get_file());
                break;
            }
            case ERR_PARSE_ERROR:
            {
                JSB_LOG(Error, "Error while parsing file '%s'.", p_file.get_file());
                break;
            }
            case ERR_FILE_CORRUPT:
            {
                JSB_LOG(Error, "%s file '%s' appears to be invalid/corrupt.", p_type, p_file.get_file());
                break;
            }
            case ERR_FILE_NOT_FOUND:
            {
                JSB_LOG(Error, "Missing file '%s' or one of its dependencies.", p_file.get_file());
                break;
            }
            case ERR_FILE_UNRECOGNIZED:
            {
                JSB_LOG(Error, "File '%s' is saved in a format that is newer than the formats supported by this version of Godot, so it can't be opened.", p_file.get_file());
                break;
            }
            default:
            {
                JSB_LOG(Error, "Error while loading file '%s'.", p_file.get_file());
                break;
            }
        }
    }
}

void GodotJSEditorHelper::_bind_methods()
{
    ClassDB::bind_static_method(jsb_typename(GodotJSEditorHelper), D_METHOD("show_toast", "text", "severity"), &GodotJSEditorHelper::show_toast);
    ClassDB::bind_static_method(jsb_typename(GodotJSEditorHelper), D_METHOD("is_file_content_equal", "path", "content"), &GodotJSEditorHelper::is_file_content_equal);
    ClassDB::bind_static_method(jsb_typename(GodotJSEditorHelper), D_METHOD("get_resource_codegen_info", "resource_path"), &GodotJSEditorHelper::get_resource_codegen_info);
    ClassDB::bind_static_method(jsb_typename(GodotJSEditorHelper), D_METHOD("get_resource_type_descriptor", "resource_path"), &GodotJSEditorHelper::get_resource_type_descriptor);
    ClassDB::bind_static_method(jsb_typename(GodotJSEditorHelper), D_METHOD("get_scene_nodes", "scene_path"), &GodotJSEditorHelper::get_scene_nodes);
}

bool GodotJSEditorHelper::is_file_content_equal(const String& p_path, const String& p_content)
{
    const String path = p_path.contains("://") ? p_path : ProjectSettings::get_singleton()->globalize_path(p_path);

    if (!FileAccess::exists(path))
    {
        return false;
    }

    Error err = OK;
    const String existing_content = FileAccess::get_file_as_string(path, &err);
    if (err != OK)
    {
        return false;
    }

    return existing_content == p_content;
}

Dictionary GodotJSEditorHelper::get_resource_type_descriptor(const String& p_path)
{
    Dictionary codegen_info = get_resource_codegen_info(p_path);
    Variant descriptor = codegen_info.get("descriptor", Dictionary());
    if (descriptor.get_type() == Variant::DICTIONARY)
    {
        return descriptor;
    }
    return Dictionary();
}

Dictionary GodotJSEditorHelper::get_resource_codegen_info(const String& p_path)
{
    Dictionary codegen_info;
    Dictionary descriptor;
    Error err;
    Ref<Resource> resource = ResourceLoader::load(p_path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &err);

    if (resource.is_null())
    {
        _log_load_error(p_path, "Resource", err);
        return codegen_info;
    }

    PackedScene* scene = Object::cast_to<PackedScene>(resource.ptr());

    if (scene)
    {
        Node* instantiated_scene = scene->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
        if (!instantiated_scene)
        {
            JSB_LOG(Error, "Error instantiating scene from %s", p_path);
            return codegen_info;
        }

        jsb::JSEnvironment env(instantiated_scene->get_scene_file_path(), true);

        Array generic_arguments;
        generic_arguments.push_back(_build_node_type_descriptor(env, instantiated_scene, p_path));

        descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::Godot;
        descriptor[jsb_string_name(name)] = "PackedScene";
        descriptor[jsb_string_name(arguments)] = generic_arguments;

        codegen_info["descriptor"] = descriptor;
        return codegen_info;
    }

    GodotJSScript* script_resource = Object::cast_to<GodotJSScript>(resource.ptr());
    ScriptInstance* script_instance = resource->get_script_instance();
    GodotJSScript* resource_script = script_instance != nullptr ? Object::cast_to<GodotJSScript>(*script_instance->get_script()) : nullptr;
    GodotJSScript* descriptor_script = resource_script != nullptr ? resource_script : script_resource;
    GodotJSScript* rpc_script = script_resource != nullptr ? script_resource : descriptor_script;
    Dictionary script_rpc_info;

    if (rpc_script != nullptr)
    {
        const String rpc_class_name = rpc_script->get_global_name();
        if (!rpc_class_name.is_empty())
        {
            const Variant rpc_config_var = rpc_script->get_rpc_config();
            if (rpc_config_var.get_type() == Variant::DICTIONARY)
            {
                const Dictionary rpc_config = rpc_config_var;
                Array method_names = rpc_config.keys();
                method_names.sort();

                Array methods;
                for (int i = 0; i < method_names.size(); i++)
                {
                    const String method_name = method_names[i];
                    if (!method_name.is_empty())
                    {
                        methods.push_back(method_name);
                    }
                }

                if (!methods.is_empty())
                {
                    script_rpc_info["class_name"] = rpc_class_name;
                    script_rpc_info["methods"] = methods;
                }
            }
        }
    }

    if (descriptor_script != nullptr)
    {
        jsb::JSEnvironment env(resource->get_path(), true);

        Dictionary codegen_request;
        codegen_request[jsb_string_name(type)] = (int32_t) CodeGenType::ScriptResourceTypeDescriptor;
        codegen_request[jsb_string_name(resource)] = resource;

        if (_request_codegen(env, descriptor_script, codegen_request, descriptor))
        {
            codegen_info["descriptor"] = descriptor;
            if (!script_rpc_info.is_empty())
            {
                codegen_info["rpc"] = script_rpc_info;
            }
            return codegen_info;
        }
    }

    if (descriptor_script == nullptr || GodotJSScriptLanguage::get_singleton()->is_global_class_generic(descriptor_script->get_path()))
    {
        descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::Godot;
        descriptor[jsb_string_name(name)] = GodotJSEditorHelper::_get_exposed_node_class_name(resource->get_class_name());
    }
    else
    {
        String class_name = descriptor_script->is_valid()
            ? static_cast<String>(descriptor_script->get_global_name())
            : ResourceLoader::get_resource_script_class(p_path);

        if (class_name.is_empty())
        {
            descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::Godot;
            descriptor[jsb_string_name(name)] = jsb::internal::NamingUtil::get_class_name("Object");
        }
        else
        {
            descriptor[jsb_string_name(type)] = (int32_t) DescriptorType::User;
            descriptor[jsb_string_name(name)] = descriptor_script->get_global_name();
            descriptor[jsb_string_name(resource)] = descriptor_script->get_path();
        }
    }

    codegen_info["descriptor"] = descriptor;
    if (!script_rpc_info.is_empty())
    {
        codegen_info["rpc"] = script_rpc_info;
    }
    return codegen_info;
}

Dictionary GodotJSEditorHelper::get_scene_nodes(const String& p_path)
{
    Error err;
    Ref<PackedScene> scene_data = ResourceLoader::load(p_path, "", ResourceFormatLoader::CACHE_MODE_REPLACE, &err);

    if (scene_data.is_null())
    {
        _log_load_error(p_path, "Resource", err);
        return Dictionary();
    }

    Node* instantiated_scene = scene_data->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);

    if (!instantiated_scene)
    {
        JSB_LOG(Error, "Error instantiating scene from %s", p_path);
        return Dictionary();
    }

    jsb::JSEnvironment env(instantiated_scene->get_scene_file_path(), true);
    v8::Isolate* isolate = env->get_isolate();
    v8::HandleScope handle_scope(isolate);

    Dictionary nodes;
    int child_count = instantiated_scene->get_child_count(true);

    for (int i = 0; i < child_count; i++)
    {
        Node* child = instantiated_scene->get_child(i, true);
        nodes[child->get_name()] = _build_node_type_descriptor(env, child);
    }

    instantiated_scene->queue_free();

    return nodes;
}

void GodotJSEditorHelper::show_toast(const String& p_text, int p_severity)
{
    EditorToaster::get_singleton()->popup_str(p_text, (EditorToaster::Severity) p_severity);
}
