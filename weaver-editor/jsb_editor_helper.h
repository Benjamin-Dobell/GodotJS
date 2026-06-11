#ifndef GODOTJS_EDITOR_HELPER_H
#define GODOTJS_EDITOR_HELPER_H
#include "jsb_editor_pch.h"

class GodotJSEditorHelper : public Object
{
    GDCLASS(GodotJSEditorHelper, Object);

private:

    static bool _request_codegen(jsb::JSEnvironment& p_env, GodotJSScript* p_script, const Dictionary& p_request, Dictionary& p_result);
    static StringName _get_exposed_node_class_name(const StringName& class_name);
    static Dictionary _build_node_type_descriptor(jsb::JSEnvironment& p_env, Node* p_node, const String& p_scene_resource_path = String());
    static void _log_load_error(const String &p_file, const String &p_type, Error p_error);

protected:
    static void _bind_methods();

public:
    virtual ~GodotJSEditorHelper() override = default;

    static bool is_file_content_equal(const String &p_path, const String &p_content);
    static Dictionary get_resource_codegen_info(const String &p_path);
    static Dictionary get_resource_type_descriptor(const String &p_path);
    static Dictionary get_scene_nodes(const String &p_path);
    static void show_toast(const String& p_text, int p_severity);
};

#endif
