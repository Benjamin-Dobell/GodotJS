#include "jsb_settings.h"

#include "jsb_internal_pch.h"
#include "jsb_macros.h"
#include "jsb_logger.h"

#define JSB_SET_RESTART(val) (val)
#define JSB_SET_IGNORE_DOCS(val) (val)
#define JSB_SET_BASIC(val) (val)
#define JSB_SET_INTERNAL(val) (val)

namespace jsb::internal
{
#ifdef TOOLS_ENABLED
    static constexpr char kEdDebuggerPort[] =     JSB_MODULE_NAME_STRING "/debugger/editor_port";
    static constexpr char kEdIgnoredClasses[] =     JSB_MODULE_NAME_STRING "/codegen/ignored_classes";
    static constexpr char kEdAutogenPath[] =     JSB_MODULE_NAME_STRING "/codegen/autogen_path";
    static constexpr char kEdAutogenSceneDTSOnSave[] =     JSB_MODULE_NAME_STRING "/codegen/autogen_scene_dts_on_save";
    static constexpr char kEdGenSceneDTS[] =     JSB_MODULE_NAME_STRING "/codegen/generate_scene_dts";
    static constexpr char kEdAutogenResourceDTSOnSave[] =     JSB_MODULE_NAME_STRING "/codegen/autogen_resource_dts_on_save";
    static constexpr char kEdGenResourceDTS[] =     JSB_MODULE_NAME_STRING "/codegen/generate_resource_dts";
    static constexpr char kEdCodegenUseProjectSettings[] =     JSB_MODULE_NAME_STRING "/codegen/use_project_settings";
#endif

    // use unnecessary first category layer (runtime and editor) to make the second layer shown as sections in project settings

    static constexpr char kRtBridgeLoggingEnabled[] = JSB_MODULE_NAME_STRING "/runtime/bridge_logging_enabled";
    static constexpr char kRtDebuggerPort[] =     JSB_MODULE_NAME_STRING "/runtime/debugger/debugger_port";
    static constexpr char kRtDebuggerSourceMapBaseUrl[] =     JSB_MODULE_NAME_STRING "/runtime/debugger/source_map_base_url";
    static constexpr char kRtDebuggerWaitForDebugger[] =     JSB_MODULE_NAME_STRING "/runtime/debugger/wait_for_debugger";
    static constexpr char kRtSourceMapEnabled[] = JSB_MODULE_NAME_STRING "/runtime/logger/source_map_enabled";
    static constexpr char kRtAdditionalSearchPaths[] = JSB_MODULE_NAME_STRING "/runtime/core/additional_search_paths";
    static constexpr char kRtEntryScriptPath[] = JSB_MODULE_NAME_STRING "/runtime/core/entry_script_path";
    static constexpr char kRtCamelCaseBindingsEnabled[] = JSB_MODULE_NAME_STRING "/runtime/core/camel_case_bindings_enabled";
#if JSB_WITH_V8
    static constexpr char kRtV8CodeCacheUnsafeUserEnabled[] = JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/unsafe_user_cache_enabled";
    static constexpr char kRtV8CodeCacheUnsafeUserPath[] = JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/unsafe_user_cache_path";
    static constexpr char kRtV8CodeCachePackagedPath[] = JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/packaged_cache_path";
    static constexpr char kRtV8CodeCacheGeneratePackaged[] = JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/generate_packaged_cache";
    static constexpr char kRtV8CodeCacheLoggingEnabled[] = JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/logging_enabled";
    static constexpr char kExV8CodeCacheEntryScene[] = JSB_MODULE_NAME_STRING "/export/v8_code_cache_entry_scene";
#endif

    // editor specific settings, but we need it configured as project-wise instead of global-wise
    static constexpr char kRtPackagingWithSourceMap[] = JSB_MODULE_NAME_STRING "/editor/packaging/source_map_included";
    static constexpr char kRtPackagingIncludeFiles[] = JSB_MODULE_NAME_STRING "/editor/packaging/include_files";
    static constexpr char kRtPackagingIncludeDirectories[] = JSB_MODULE_NAME_STRING "/editor/packaging/include_directories";
    static constexpr char kRtPackagingStaticAssets[] = JSB_MODULE_NAME_STRING "/editor/packaging/static_assets";
    static constexpr char kRtPackagingReferencedNodeModules[] = JSB_MODULE_NAME_STRING "/editor/packaging/referenced_node_modules";
    static constexpr char kRtPackagingOutputDirectory[] = JSB_MODULE_NAME_STRING "/editor/packaging/output_directory";
    static constexpr char kRtPackagingTypescriptBuildCommand[] = JSB_MODULE_NAME_STRING "/editor/packaging/typescript_build_command";

    // Keep the pointer constant-initialized: a file-scope String/Vector would
    // touch the allocator during startup before Godot's allocation profiler is
    // ready. The stack is thread-local so export-time path selection cannot
    // leak into another runtime thread, and push/pop supports nested exports.
    static thread_local Vector<String>* export_jsb_out_res_path_stack = nullptr;

    String normalize_jsb_output_directory(const Variant& p_value, const String& p_default)
    {
        if (p_value.get_type() != Variant::STRING)
        {
            return p_default;
        }

        String path = String(p_value).strip_edges().replace("\\", "/").simplify_path();
        if (path.begins_with("res://"))
        {
            path = path.substr(6);
        }
        if (path.is_empty())
        {
            return p_default;
        }
        if (!Settings::validate_jsb_out_dir_name(path))
        {
            return p_default;
        }
        return path;
    }


#ifdef TOOLS_ENABLED
    bool init_editor_settings()
    {
        static bool inited = false;
        if (!inited)
        {
            if (!EditorSettings::get_singleton())
            {
                if (Engine::get_singleton()->is_editor_hint() || Engine::get_singleton()->is_project_manager_hint() || jsb_ext_is_cmdline_tool())
                {
                    EditorSettings::create();
                    jsb_check(EditorSettings::get_singleton());
                }
                else
                {
                    JSB_LOG(Verbose, "EditorSettings is not available when initialising %s", jsb_typename(jsb::internal::Settings));
                }
            }

            // check before read to avoid redundant warnings
            if (EditorSettings::get_singleton())
            {
                inited = true;
                _EDITOR_DEF(kEdDebuggerPort, 9230, true);
                _EDITOR_DEF(kEdIgnoredClasses, PackedStringArray(), false);
                _EDITOR_DEF(kEdAutogenPath, "gen/godot", false);
                _EDITOR_DEF(kEdGenSceneDTS, true, false);
                _EDITOR_DEF(kEdAutogenSceneDTSOnSave, true, false);
                _EDITOR_DEF(kEdGenResourceDTS, true, false);
                _EDITOR_DEF(kEdAutogenResourceDTSOnSave, true, false);
                _EDITOR_DEF(kEdCodegenUseProjectSettings, true, false);
            }
        }
        return inited;
    }
#endif

    void init_settings()
    {
        static bool inited = false;
        if (!inited)
        {
            inited = true;
            static constexpr char filter[] = "*." JSB_JAVASCRIPT_EXT ",*." JSB_COMMONJS_EXT ",*." JSB_MODULE_EXT
#if JSB_USE_TYPESCRIPT
                                            ",*." JSB_TYPESCRIPT_EXT
#endif
            ;
            _GLOBAL_DEF(kRtDebuggerPort, 9229, JSB_SET_RESTART(true), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(false), JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtDebuggerSourceMapBaseUrl, "http://localhost:9230", JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(false), JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtDebuggerWaitForDebugger, false, JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtSourceMapEnabled, true, JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtAdditionalSearchPaths, PackedStringArray(), JSB_SET_RESTART(false),  JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtCamelCaseBindingsEnabled, false, JSB_SET_RESTART(true), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
#if JSB_WITH_V8
            _GLOBAL_DEF(kRtV8CodeCacheUnsafeUserEnabled, false, JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true), JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtV8CodeCacheUnsafeUserPath, "user://GodotJSCodeCache/v8", JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(false), JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtV8CodeCachePackagedPath, "res://.godotjs/v8_code_cache", JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(false), JSB_SET_INTERNAL(false));
            _GLOBAL_DEF(kRtV8CodeCacheGeneratePackaged, false, JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(true), JSB_SET_BASIC(false), JSB_SET_INTERNAL(true));
            _GLOBAL_DEF(kRtV8CodeCacheLoggingEnabled, false, JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(false), JSB_SET_INTERNAL(false));
#endif

            {
                PropertyInfo EntryScriptPath;
                EntryScriptPath.type = Variant::STRING;
                EntryScriptPath.name = kRtEntryScriptPath;
                EntryScriptPath.hint = PROPERTY_HINT_FILE;
                EntryScriptPath.hint_string = filter;
                _GLOBAL_DEF(EntryScriptPath, String(), JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            }

#if JSB_WITH_V8
            {
                PropertyInfo EntryScenePath;
                EntryScenePath.type = Variant::STRING;
                EntryScenePath.name = kExV8CodeCacheEntryScene;
                EntryScenePath.hint = PROPERTY_HINT_FILE;
                EntryScenePath.hint_string = "*.tscn";
                _GLOBAL_DEF(EntryScenePath, String(), JSB_SET_RESTART(false), JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true), JSB_SET_INTERNAL(false));
            }
#endif

            _GLOBAL_DEF(kRtPackagingWithSourceMap, true, false);

            {
                PropertyInfo PackagingOutputDirectory;
                PackagingOutputDirectory.type = Variant::STRING;
                PackagingOutputDirectory.name = kRtPackagingOutputDirectory;
                PackagingOutputDirectory.hint = PROPERTY_HINT_DIR;
                const String default_output_directory = ProjectSettings::get_singleton()->get_project_data_dir_name().path_join(JSB_MODULE_NAME_STRING);
                _GLOBAL_DEF(PackagingOutputDirectory, default_output_directory, false, JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true), JSB_SET_INTERNAL(false));
            }

            {
                PropertyInfo PackagingTypescriptBuildCommand;
                PackagingTypescriptBuildCommand.type = Variant::PACKED_STRING_ARRAY;
                PackagingTypescriptBuildCommand.name = kRtPackagingTypescriptBuildCommand;
                PackagingTypescriptBuildCommand.hint = PROPERTY_HINT_ARRAY_TYPE;
                PackagingTypescriptBuildCommand.hint_string = "String";
                _GLOBAL_DEF(PackagingTypescriptBuildCommand, PackedStringArray(), false, JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true), JSB_SET_INTERNAL(false));
            }

            {
                PropertyInfo PackagingIncludeFiles;
                PackagingIncludeFiles.type = Variant::ARRAY;
                PackagingIncludeFiles.name = kRtPackagingIncludeFiles;
                PackagingIncludeFiles.hint = PROPERTY_HINT_ARRAY_TYPE;
                PackagingIncludeFiles.hint_string = vformat("%s/%s:%s", Variant::STRING, PROPERTY_HINT_FILE, filter);
                _GLOBAL_DEF(PackagingIncludeFiles, Array(), false, JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            }

            {
                PropertyInfo PackagingIncludeDirectories;
                PackagingIncludeDirectories.type = Variant::ARRAY;
                PackagingIncludeDirectories.name = kRtPackagingIncludeDirectories;
                PackagingIncludeDirectories.hint = PROPERTY_HINT_ARRAY_TYPE;
                PackagingIncludeDirectories.hint_string = vformat("%s/%s:%s", Variant::STRING, PROPERTY_HINT_DIR, filter);
                _GLOBAL_DEF(PackagingIncludeDirectories, Array(), false, JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            }

            {
                PropertyInfo PackagingStaticAssets;
                PackagingStaticAssets.type = Variant::ARRAY;
                PackagingStaticAssets.name = kRtPackagingStaticAssets;
                PackagingStaticAssets.hint = PROPERTY_HINT_ARRAY_TYPE;
                PackagingStaticAssets.hint_string = vformat("%s", Variant::STRING);
                _GLOBAL_DEF(PackagingStaticAssets, Array(), false, JSB_SET_IGNORE_DOCS(false), JSB_SET_BASIC(true),  JSB_SET_INTERNAL(false));
            }

            _GLOBAL_DEF(kRtPackagingReferencedNodeModules, true, false);
            _GLOBAL_DEF(kRtBridgeLoggingEnabled, false, false);
        }
    }

#ifdef TOOLS_ENABLED
    bool Settings::editor_settings_available()
    {
        return EditorSettings::get_singleton() != nullptr || Engine::get_singleton()->is_editor_hint() || Engine::get_singleton()->is_project_manager_hint() || jsb_ext_is_cmdline_tool();
    }

    PackedStringArray Settings::get_ignored_classes()
    {
        init_editor_settings();
        return EDITOR_GET(kEdIgnoredClasses);
    }

    String Settings::get_autogen_path()
    {
        init_editor_settings();
        return EDITOR_GET(kEdAutogenPath);
    }

    bool Settings::get_autogen_scene_dts_on_save()
    {
        init_editor_settings();
        return EDITOR_GET(kEdAutogenSceneDTSOnSave);
    }

    bool Settings::get_gen_scene_dts()
    {
        init_editor_settings();
        return EDITOR_GET(kEdGenSceneDTS);
    }

    bool Settings::get_autogen_resource_dts_on_save()
    {
        init_editor_settings();
        return EDITOR_GET(kEdAutogenResourceDTSOnSave);
    }

    bool Settings::get_gen_resource_dts()
    {
        init_editor_settings();
        return EDITOR_GET(kEdGenResourceDTS);
    }

    bool Settings::get_codegen_use_project_settings()
    {
        init_editor_settings();
        return EDITOR_GET(kEdCodegenUseProjectSettings);
    }
#endif

    bool Settings::is_packaging_with_source_map()
    {
        init_settings();
        return GLOBAL_GET(kRtPackagingWithSourceMap);
    }

    PackedStringArray Settings::get_packaging_include_files()
    {
        init_settings();
        // rely on auto variant convert from Array
        return (PackedStringArray) GLOBAL_GET(kRtPackagingIncludeFiles);
    }

    PackedStringArray Settings::get_packaging_include_directories()
    {
        init_settings();
        // rely on auto variant convert from Array
        return (PackedStringArray) GLOBAL_GET(kRtPackagingIncludeDirectories);
    }

    PackedStringArray Settings::get_packaging_static_assets()
    {
        init_settings();
        // rely on auto variant convert from Array
        return (PackedStringArray) GLOBAL_GET(kRtPackagingStaticAssets);
    }

    bool Settings::is_packaging_referenced_node_modules()
    {
        init_settings();
        return GLOBAL_GET(kRtPackagingReferencedNodeModules);
    }

    PackedStringArray Settings::get_packaging_typescript_build_command()
    {
        init_settings();
        return (PackedStringArray) GLOBAL_GET(kRtPackagingTypescriptBuildCommand);
    }

    PackedStringArray Settings::get_packaging_typescript_build_command(const Vector<String>& p_features)
    {
        init_settings();
        return (PackedStringArray) ProjectSettings::get_singleton()->get_setting_with_override_and_custom_features(
            kRtPackagingTypescriptBuildCommand,
            p_features
        );
    }

    bool Settings::is_bridge_logging_enabled()
    {
        init_settings();
        return GLOBAL_GET(kRtBridgeLoggingEnabled);
    }

    uint16_t Settings::get_debugger_port()
    {
#ifdef TOOLS_ENABLED
        if (Engine::get_singleton()->is_editor_hint())
        {
            init_editor_settings();
            return EDITOR_GET(kEdDebuggerPort);
        }
#endif
        init_settings();
        return GLOBAL_GET(kRtDebuggerPort);
    }

    String Settings::get_debugger_source_map_base_url()
    {
        init_settings();
        return GLOBAL_GET(kRtDebuggerSourceMapBaseUrl);
    }

    bool Settings::get_wait_for_debugger()
    {
        init_settings();
        return GLOBAL_GET(kRtDebuggerWaitForDebugger);
    }

    bool Settings::get_sourcemap_enabled()
    {
        init_settings();
        return GLOBAL_GET(kRtSourceMapEnabled);
    }

    String Settings::get_project_data_dir_name()
    {
        const String project_data_dir = ProjectSettings::get_singleton()->get_project_data_dir_name();
        jsb_check(!project_data_dir.is_empty());
        return project_data_dir;
    }

    String Settings::get_jsb_out_dir_name()
    {
        init_settings();
        const String default_path = get_project_data_dir_name().path_join(JSB_MODULE_NAME_STRING);
        return normalize_jsb_output_directory(GLOBAL_GET(kRtPackagingOutputDirectory), default_path);
    }

    bool Settings::validate_jsb_out_dir_name(const String& p_path, String* r_error)
    {
        // Validate the unsimplified component sequence. `String::simplify_path()`
        // collapses an escaping sequence such as `x/../../outside` to a safe-
        // looking path before validation, which would allow the exporter to
        // launch its build command for an output that is outside the project.
        String path = p_path.strip_edges().replace("\\", "/");
        if (path.begins_with("res://"))
        {
            path = path.substr(6);
        }
        if (path.is_empty())
        {
            return true;
        }
        bool escapes_project_root = path.begins_with("/") || path.contains(":/");
        int component_depth = 0;
        const Vector<String> components = path.split("/");
        for (const String &component : components)
        {
            if (component.is_empty() || component == ".")
            {
                continue;
            }
            if (component == "..")
            {
                if (component_depth == 0)
                {
                    escapes_project_root = true;
                    break;
                }
                --component_depth;
            }
            else
            {
                ++component_depth;
            }
        }
        if (path == "." || path == ".." || path.begins_with("../") || escapes_project_root || component_depth == 0)
        {
            if (r_error)
            {
                *r_error = "GodotJS/editor/packaging/output_directory must be empty or a project-relative path under res:// (for example .godot/GodotJS/web-export).";
            }
            return false;
        }
        return true;
    }

    String Settings::get_jsb_out_dir_name(const Vector<String>& p_features)
    {
        init_settings();
        const String default_path = get_project_data_dir_name().path_join(JSB_MODULE_NAME_STRING);
        const Variant configured = ProjectSettings::get_singleton()->get_setting_with_override_and_custom_features(
            kRtPackagingOutputDirectory,
            p_features
        );
        return normalize_jsb_output_directory(configured, default_path);
    }

    String Settings::get_tsbuildinfo_path()
    {
        return get_project_data_dir_name().path_join(".tsbuildinfo");
    }

    String Settings::get_jsb_out_res_path()
    {
        if (export_jsb_out_res_path_stack && !export_jsb_out_res_path_stack->is_empty())
        {
            return export_jsb_out_res_path_stack->get(export_jsb_out_res_path_stack->size() - 1);
        }
        return "res://" + get_jsb_out_dir_name();
    }

    void Settings::set_export_jsb_out_res_path(const String& p_out_res_path)
    {
        if (!export_jsb_out_res_path_stack)
        {
            export_jsb_out_res_path_stack = new Vector<String>();
        }
        export_jsb_out_res_path_stack->push_back(p_out_res_path);
    }

    void Settings::clear_export_jsb_out_res_path()
    {
        if (export_jsb_out_res_path_stack && !export_jsb_out_res_path_stack->is_empty())
        {
            export_jsb_out_res_path_stack->resize(export_jsb_out_res_path_stack->size() - 1);
            if (export_jsb_out_res_path_stack->is_empty())
            {
                delete export_jsb_out_res_path_stack;
                export_jsb_out_res_path_stack = nullptr;
            }
        }
    }

    String Settings::get_jsb_out_res_path(const Vector<String>& p_features)
    {
        return "res://" + get_jsb_out_dir_name(p_features);
    }

    PackedStringArray Settings::get_additional_search_paths()
    {
        init_settings();
        return GLOBAL_GET(kRtAdditionalSearchPaths);
    }

    String Settings::get_entry_script_path()
    {
        init_settings();
        return GLOBAL_GET(kRtEntryScriptPath);
    }

    bool Settings::get_camel_case_bindings_enabled()
    {
        init_settings();
        static bool initialized = false;
        static bool cached_enabled = false;
        if (!initialized)
        {
            cached_enabled = GLOBAL_GET(kRtCamelCaseBindingsEnabled);
            initialized = true;
        }
        return cached_enabled;
    }

#if JSB_WITH_V8
    bool Settings::get_v8_code_cache_unsafe_user_enabled()
    {
        init_settings();
        return GLOBAL_GET(kRtV8CodeCacheUnsafeUserEnabled);
    }

    String Settings::get_v8_code_cache_unsafe_user_path()
    {
        init_settings();
        return GLOBAL_GET(kRtV8CodeCacheUnsafeUserPath);
    }

    String Settings::get_v8_code_cache_packaged_path()
    {
        init_settings();
        return GLOBAL_GET(kRtV8CodeCachePackagedPath);
    }

    bool Settings::is_v8_code_cache_packaged_generation_enabled()
    {
        init_settings();
        return GLOBAL_GET(kRtV8CodeCacheGeneratePackaged);
    }

    bool Settings::is_v8_code_cache_logging_enabled()
    {
        init_settings();
        return GLOBAL_GET(kRtV8CodeCacheLoggingEnabled);
    }

    String Settings::get_v8_code_cache_entry_scene()
    {
        init_settings();
        return GLOBAL_GET(kExV8CodeCacheEntryScene);
    }
#endif

    String Settings::get_indentation()
    {
#ifdef TOOLS_ENABLED
        if (Engine::get_singleton()->is_editor_hint())
        {
            init_settings();
            // use_space_indentation
            if (!!EDITOR_GET("text_editor/behavior/indent/type"))
            {
                const int indent_size = EDITOR_GET("text_editor/behavior/indent/size");
                return String(" ").repeat(indent_size);
            }
        }
#endif
        return "\t";
    }

}
