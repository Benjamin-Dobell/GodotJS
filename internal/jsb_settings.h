#ifndef GODOTJS_SETTINGS_H
#define GODOTJS_SETTINGS_H

#include <cstdint>
#include "../compat/jsb_compat.h"

namespace jsb::internal
{
    class Settings
    {
    public:
        static bool is_bridge_logging_enabled();
        static uint16_t get_debugger_port();
        static String get_debugger_source_map_base_url();
        static bool get_wait_for_debugger();
        static bool get_sourcemap_enabled();

        /**
         * get the project relative path for `outDir` (it refers to `.godot/GodotJS` by default)
         */
        static String get_jsb_out_dir_name();

        /**
         * Resolve the output directory using an explicit export feature set.
         * This is used by the editor exporter because the host OS is not the
         * exported runtime platform.
         */
        static String get_jsb_out_dir_name(const Vector<String>& p_features);

        /** Validate a project-relative output directory; empty selects the automatic default. */
        static bool validate_jsb_out_dir_name(const String& p_path, String* r_error = nullptr);

        /**
         * get path for .tsbuildinfo (.godot/.tsbuildinfo)
         */
        static String get_tsbuildinfo_path();

        /**
         * Temporarily select the output root used while an export plugin loads
         * compiled modules. Export-time module/resource callbacks otherwise run
         * on the editor host and cannot infer the preset's custom feature set.
         */
        static void set_export_jsb_out_res_path(const String& p_out_res_path);
        static void clear_export_jsb_out_res_path();

        /**
         * get the res path for `outDir`, it's equivalent to `res://` + get_jsb_out_dir_name()
         */
        static String get_jsb_out_res_path();

        /** Resolve the output path using an explicit export feature set. */
        static String get_jsb_out_res_path(const Vector<String>& p_features);

        static String get_indentation();

        static String get_project_data_dir_name();

        static PackedStringArray get_additional_search_paths();

        static String get_entry_script_path();

        static bool get_camel_case_bindings_enabled();

#if JSB_WITH_V8
        static bool get_v8_code_cache_unsafe_user_enabled();

        static String get_v8_code_cache_unsafe_user_path();

        static String get_v8_code_cache_packaged_path();

        static bool is_v8_code_cache_packaged_generation_enabled();

        static bool is_v8_code_cache_logging_enabled();

        static String get_v8_code_cache_entry_scene();
#endif

        static bool is_packaging_with_source_map();

        static PackedStringArray get_packaging_include_files();

        static PackedStringArray get_packaging_include_directories();

        static PackedStringArray get_packaging_static_assets();

        static bool is_packaging_referenced_node_modules();

        static PackedStringArray get_packaging_typescript_build_command();
        static PackedStringArray get_packaging_typescript_build_command(const Vector<String>& p_features);

#ifdef TOOLS_ENABLED
        // [EDITOR ONLY]
        static bool editor_settings_available();
        static PackedStringArray get_ignored_classes();
        static String get_autogen_path();
        static bool get_autogen_scene_dts_on_save();
        static bool get_gen_scene_dts();
        static bool get_autogen_resource_dts_on_save();
        static bool get_gen_resource_dts();
        static bool get_codegen_use_project_settings();
#endif
    };
}

#endif
