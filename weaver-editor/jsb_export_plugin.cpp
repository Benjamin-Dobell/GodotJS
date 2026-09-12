#include "jsb_export_plugin.h"

#include "../internal/jsb_web_commonjs_manifest.h"
#include "../weaver/jsb_script.h"
#include "core/io/config_file.h"
#include "core/io/json.h"

#define JSB_EXPORTER_LOG(Severity, Format, ...) JSB_LOG_IMPL(JSExporter, Severity, Format, ##__VA_ARGS__)

namespace
{
    static constexpr char kWebCommonJSExportOption[] = JSB_MODULE_NAME_STRING "/web_commonjs/enabled";
    static constexpr char kPackagingOutputDirectorySetting[] = JSB_MODULE_NAME_STRING "/editor/packaging/output_directory";
    static constexpr char kCommonJSWrapperHeader[] = "module.exports = function(exports,require,module,__filename,__dirname){";
    static constexpr char kCommonJSWrapperFooter[] = "\n};\n";

#if JSB_WITH_V8
    static constexpr char kGeneratedV8OverridePath[] = "res://.godotjs_v8_export_override.cfg";

    void set_project_setting(Ref<ConfigFile>& p_config, const String& p_setting_path, const Variant& p_value)
    {
        const int slash_index = p_setting_path.find_char('/');
        ERR_FAIL_COND(slash_index < 0);
        p_config->set_value(p_setting_path.substr(0, slash_index), p_setting_path.substr(slash_index + 1), p_value);
    }

    String get_top_level_res_component(const String& p_res_path)
    {
        if (!p_res_path.begins_with("res://"))
        {
            return String();
        }

        const String relative_path = p_res_path.substr(6);
        const int slash_index = relative_path.find_char('/');
        return slash_index >= 0 ? relative_path.substr(0, slash_index) : relative_path;
    }

    String get_res_relative_path(const String& p_res_path)
    {
        return p_res_path.begins_with("res://") ? p_res_path.substr(6) : String();
    }

    bool remove_path_recursive_absolute(const String& p_path)
    {
        if (p_path.is_empty())
        {
            return false;
        }

        if (DirAccess::dir_exists_absolute(p_path))
        {
            Ref<DirAccess> dir = DirAccess::open(p_path);
            if (dir.is_null() || dir->erase_contents_recursive() != OK)
            {
                return false;
            }
            return DirAccess::remove_absolute(p_path) == OK;
        }

        if (FileAccess::exists(p_path))
        {
            return DirAccess::remove_absolute(p_path) == OK;
        }

        return true;
    }

    bool mirror_project_root(const String& p_source_root, const String& p_mirror_root, const String& p_packaged_cache_root)
    {
        Ref<DirAccess> source_dir = DirAccess::open(p_source_root);
        Ref<DirAccess> mirror_dir = DirAccess::open(p_mirror_root);
        ERR_FAIL_COND_V(source_dir.is_null() || mirror_dir.is_null(), false);

        const String packaged_cache_relative_path = get_res_relative_path(p_packaged_cache_root);
        const String packaged_cache_top_level = get_top_level_res_component(p_packaged_cache_root);
        const String generated_override_filename = String(kGeneratedV8OverridePath).substr(6);
        source_dir->set_include_hidden(true);
        source_dir->list_dir_begin();
        String entry = source_dir->get_next();
        while (!entry.is_empty())
        {
            if (entry != "." && entry != ".." && entry != "override.cfg" && entry != generated_override_filename)
            {
                const String source_path = p_source_root.path_join(entry);
                const String mirror_path = p_mirror_root.path_join(entry);
                const bool is_packaged_cache_top_level = !packaged_cache_top_level.is_empty() && entry == packaged_cache_top_level;
                Error err = is_packaged_cache_top_level ? ERR_CANT_CREATE : mirror_dir->create_link(source_path, mirror_path);
                if (err != OK)
                {
                    err = source_dir->current_is_dir()
                        ? mirror_dir->copy_dir(source_path, mirror_path, -1, true)
                        : mirror_dir->copy(source_path, mirror_path);
                }
                if (err != OK)
                {
                    source_dir->list_dir_end();
                    return false;
                }
            }
            entry = source_dir->get_next();
        }
        source_dir->list_dir_end();
        return packaged_cache_relative_path.is_empty() ||
            remove_path_recursive_absolute(p_mirror_root.path_join(packaged_cache_relative_path));
    }

    bool write_v8_export_override_files(const String& p_source_root, const String& p_mirror_root, const String& p_packaged_cache_root)
    {
        const String mirror_override_path = p_mirror_root.path_join("override.cfg");
        const String source_override_path = p_source_root.path_join("override.cfg");

        Ref<ConfigFile> override_config;
        override_config.instantiate();
        if (FileAccess::exists(source_override_path))
        {
            const Error load_err = override_config->load(source_override_path);
            if (load_err != OK)
            {
                return false;
            }
        }
        set_project_setting(override_config, "application/config/project_settings_override", String(kGeneratedV8OverridePath));
        if (override_config->save(mirror_override_path) != OK)
        {
            return false;
        }

        Ref<ConfigFile> helper_override;
        helper_override.instantiate();
        set_project_setting(helper_override, JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/unsafe_user_cache_enabled", false);
        set_project_setting(helper_override, JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/generate_packaged_cache", true);
        set_project_setting(helper_override, JSB_MODULE_NAME_STRING "/runtime/v8_code_cache/packaged_cache_path", p_packaged_cache_root);
        const String helper_override_path = p_mirror_root.path_join(String(kGeneratedV8OverridePath).substr(6));
        return helper_override->save(helper_override_path) == OK;
    }
#endif
}

HashSet<String> GodotJSExportPlugin::ignored_paths_ {
    String("res://jsconfig.json"),
    String("res://tsconfig.json"),
    String("res://package.json"),
    String("res://package-lock.json"),
};

GodotJSExportPlugin::GodotJSExportPlugin() : super()
{
    env_ = GodotJSScriptLanguage::get_singleton()->get_environment();
    jsb_check(env_);
}

PackedStringArray GodotJSExportPlugin::_get_export_features(const Ref<EditorExportPlatform>& p_export_platform, bool p_debug) const
{
    // if (FileAccess::exists("res://tsconfig.json"))
    // {
    //     return { "typescript" };
    // }
    return {};
}

void GodotJSExportPlugin::_get_export_options(const Ref<EditorExportPlatform>& p_export_platform, List<EditorExportPlatform::ExportOption>* r_options) const
{
    r_options->push_back(EditorExportPlatform::ExportOption(PropertyInfo(Variant::BOOL, kWebCommonJSExportOption), false, true));
}

bool GodotJSExportPlugin::_get_export_option_visibility(const Ref<EditorExportPlatform>& p_export_platform, const String& p_option_name) const
{
    if (p_option_name == kWebCommonJSExportOption)
    {
        return is_web_platform(p_export_platform);
    }
    return true;
}

bool GodotJSExportPlugin::is_web_platform(const Ref<EditorExportPlatform>& p_export_platform)
{
    return p_export_platform.is_valid() && p_export_platform->get_name().to_lower() == "web";
}

bool GodotJSExportPlugin::is_javascript_like_file(const String& p_path)
{
    return jsb::internal::PathUtil::is_recognized_javascript_extension(p_path);
}

bool GodotJSExportPlugin::is_commonjs_external_file(const String& p_path)
{
    return is_javascript_like_file(p_path) || p_path.ends_with("." JSB_JSON_EXT);
}

String GodotJSExportPlugin::normalize_static_asset_path(const String& p_path)
{
    String normalized_path = p_path.strip_edges().replace("\\", "/");
    if (normalized_path.is_empty())
    {
        return String();
    }

    if (!normalized_path.begins_with("res://"))
    {
        while (normalized_path.begins_with("/"))
        {
            normalized_path = normalized_path.substr(1);
        }
        normalized_path = "res://" + normalized_path;
    }

    while (normalized_path.length() > 6 && normalized_path.ends_with("/"))
    {
        normalized_path = normalized_path.substr(0, normalized_path.length() - 1);
    }

    return normalized_path;
}

String GodotJSExportPlugin::get_res_relative_path(const String& p_res_path)
{
    return p_res_path.begins_with("res://") ? p_res_path.substr(6) : String();
}

bool GodotJSExportPlugin::is_web_commonjs_export_enabled() const
{
    return web_commonjs_export_;
}

bool GodotJSExportPlugin::is_static_asset_path(const String& p_path) const
{
    const String normalized_path = normalize_static_asset_path(p_path);
    if (normalized_path.is_empty())
    {
        return false;
    }

    return static_asset_paths_.has(normalized_path);
}

void GodotJSExportPlugin::export_raw_files(const PackedStringArray &p_paths, bool p_permit_typescript)
{
    if (export_build_failed_)
    {
        return;
    }

    for (const String& file_path : p_paths)
    {
        // in this situation, we do not call `load module` to avoid unexpected side effects
        // (for example, it's impossible to directly load worker scripts in main env).

        if (!file_path.ends_with("." JSB_TYPESCRIPT_EXT))
        {
            bool exported = false;
            if (is_web_commonjs_export_enabled() && file_path.get_file() == "package.json")
            {
                write_external_file_as(file_path, file_path, false);
                exported = export_raw_file(file_path, false);
            }
            else
            {
                exported = export_external_or_raw_file(file_path, false);
            }
            if (!exported)
            {
                get_export_platform()->add_message(
                    EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                    get_name(),
                    vformat("Failed to export required GodotJS file: %s", file_path));
            }
        }
        else if (p_permit_typescript)
        {
            const String compiled_script_path = jsb::internal::PathUtil::convert_typescript_path(file_path, export_output_res_path_);
            if (export_dedicated_build_enabled_ && !FileAccess::exists(compiled_script_path))
            {
                JSB_EXPORTER_LOG(Verbose, "skip TypeScript include without a compiled Web/export graph entry: %s", file_path);
                continue;
            }
            if (!export_external_or_raw_file(compiled_script_path, true))
            {
                get_export_platform()->add_message(
                    EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                    get_name(),
                    vformat("Failed to export required compiled GodotJS file: %s", compiled_script_path));
            }
        }
    }
}

void GodotJSExportPlugin::get_script_resources(const String &p_dir, Vector<String> &r_list, bool p_is_node_module)
{
    Ref<DirAccess> dir = DirAccess::open(p_dir);

    if (!dir.is_valid())
    {
        JSB_EXPORTER_LOG(Warning, "Could not open explicit script directory for traversal: %s", p_dir);
        return;
    }

    dir->list_dir_begin();
    String filename = dir->get_next();

    while (!filename.is_empty())
    {
        if (filename == "." || filename == ".." || (p_is_node_module && filename == "node_modules"))
        {
            filename = dir->get_next();
            continue;
        }

        String path = p_dir.path_join(filename);

        if (dir->current_is_dir())
        {
            get_script_resources(path, r_list, p_is_node_module);
        }
        else if (!get_ignored_paths().has(path)
            && (ResourceLoader::get_resource_type(path) == jsb_typename(GodotJSScript)
                || (p_is_node_module && filename == "package.json")))
        {
            r_list.push_back(path);
        }

        filename = dir->get_next();
    }

    dir->list_dir_end();
}

// p_path is the exported package full path (like X:/Folder1/Folder2/test.zip)
void GodotJSExportPlugin::_export_begin(const HashSet<String>& p_features, bool p_debug, const String& p_path, int p_flags)
{
    JSB_EXPORTER_LOG(Verbose, "export_begin path: %s", p_path);
    exported_paths_.clear();
    external_paths_.clear();
    static_asset_paths_.clear();
    default_output_res_path_ = jsb::internal::Settings::get_jsb_out_res_path();
    export_build_failed_ = false;
    export_dedicated_build_enabled_ = false;
    export_output_path_pushed_ = false;
    Vector<String> export_features;
    export_features.reserve(p_features.size());
    for (const String& feature : p_features)
    {
        export_features.push_back(feature);
    }
    const Variant configured_output_directory = ProjectSettings::get_singleton()->get_setting_with_override_and_custom_features(
        kPackagingOutputDirectorySetting,
        export_features
    );
    if (configured_output_directory.get_type() != Variant::NIL
        && configured_output_directory.get_type() != Variant::STRING)
    {
        export_build_failed_ = true;
        get_export_platform()->add_message(
            EditorExportPlatform::EXPORT_MESSAGE_ERROR,
            get_name(),
            "GodotJS/editor/packaging/output_directory must be a string path or empty."
        );
    }
    else if (configured_output_directory.get_type() == Variant::STRING)
    {
        String output_directory_error;
        if (!jsb::internal::Settings::validate_jsb_out_dir_name(String(configured_output_directory), &output_directory_error))
        {
            export_build_failed_ = true;
            get_export_platform()->add_message(
                EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                get_name(),
                output_directory_error
            );
        }
    }
    export_output_res_path_ = jsb::internal::Settings::get_jsb_out_res_path(export_features);
    if (!export_build_failed_)
    {
        jsb::internal::Settings::set_export_jsb_out_res_path(export_output_res_path_);
        export_output_path_pushed_ = true;
    }
    JSB_EXPORTER_LOG(Verbose, "TypeScript export output path: %s", export_output_res_path_);
    const PackedStringArray typescript_build_command = jsb::internal::Settings::get_packaging_typescript_build_command(export_features);
    if (!export_build_failed_ && typescript_build_command.size() > 0)
    {
        export_dedicated_build_enabled_ = true;
        String executable = typescript_build_command.get(0);
        List<String> arguments;
        for (int index = 1; index < typescript_build_command.size(); index++)
        {
            String argument = typescript_build_command.get(index);
            if (argument.begins_with("res://"))
            {
                argument = ProjectSettings::get_singleton()->globalize_path(argument);
            }
            arguments.push_back(argument);
        }
        if (executable.begins_with("res://"))
        {
            executable = ProjectSettings::get_singleton()->globalize_path(executable);
        }

        String build_output;
        int build_exit_code = 0;
        const Error build_error = OS::get_singleton()->execute(
            executable,
            arguments,
            &build_output,
            &build_exit_code,
            true
        );
        if (build_error != OK || build_exit_code != 0)
        {
            export_build_failed_ = true;
            get_export_platform()->add_message(
                EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                get_name(),
                vformat(
                    "Configured TypeScript build failed (error=%s, exit_code=%d): %s",
                    jsb_ext_error_string(build_error),
                    build_exit_code,
                    build_output.strip_edges()
                )
            );
        }
        else
        {
            // The exporter shares the editor's main JS environment.  A
            // repeated export can therefore otherwise traverse the module
            // cache populated by an earlier export, even though the
            // synchronous TypeScript build above has emitted fresh bytes at
            // the same paths.  Use the normal editor hot-reload pipeline so
            // changed modules and their transitive dependents are marked,
            // reloaded, and any live GodotJSScript instances are rebound
            // before dependency traversal begins.
            if (GodotJSScriptLanguage* language = GodotJSScriptLanguage::get_singleton())
            {
                const Error reload_error = language->scan_external_changes();
                if (reload_error != OK)
                {
                    export_build_failed_ = true;
                    get_export_platform()->add_message(
                        EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                        get_name(),
                        vformat(
                            "Failed to reload GodotJS modules after the TypeScript build (error=%s).",
                            jsb_ext_error_string(reload_error)
                        )
                    );
                }
            }
            else
            {
                export_build_failed_ = true;
                get_export_platform()->add_message(
                    EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                    get_name(),
                    "GodotJS script language is unavailable after the TypeScript build."
                );
            }
        }
    }
    web_commonjs_export_ = p_features.has("web") && (bool) get_option(kWebCommonJSExportOption);
    web_commonjs_export_base_dir_ = web_commonjs_export_ ? p_path.get_base_dir() : String();
    const PackedStringArray static_assets = jsb::internal::Settings::get_packaging_static_assets();
    for (const String& static_asset_path : static_assets)
    {
        export_static_asset_path(static_asset_path);
    }

    if (web_commonjs_export_)
    {
        JSB_EXPORTER_LOG(Verbose, "Web CommonJS export enabled. Loose scripts will be written under %s", web_commonjs_export_base_dir_);
    }

    // add all explicitly included file paths in settings
    const PackedStringArray file_paths = jsb::internal::Settings::get_packaging_include_files();
    export_raw_files(file_paths, true);

    // add all explicitly included directory paths
    const PackedStringArray dir_paths = jsb::internal::Settings::get_packaging_include_directories();
    for (const String& dir_path : dir_paths)
    {
        Vector<String> script_paths;
        get_script_resources(dir_path, script_paths);
        export_raw_files(script_paths, true);
    }

#if JSB_WITH_V8
    bool exported_packaged_cache = false;
    const String entry_scene = jsb::internal::Settings::get_v8_code_cache_entry_scene();
    JSB_EXPORTER_LOG(Verbose, "V8 packaged code cache export entry scene: %s", entry_scene);
    if (!entry_scene.is_empty())
    {
        exported_packaged_cache = generate_and_export_v8_packaged_cache(entry_scene);
    }

    if (exported_packaged_cache)
    {
        get_export_platform()->add_message(
            EditorExportPlatform::EXPORT_MESSAGE_WARNING,
            get_name(),
            "GodotJS V8 packaged code cache trusts the exported pack. On Windows/Linux, prefer embedding the PCK into the executable or external pack verification. On macOS, prefer a signed app bundle.");
    }
#endif
}

void GodotJSExportPlugin::_export_end()
{
    if (export_output_path_pushed_)
    {
        jsb::internal::Settings::clear_export_jsb_out_res_path();
        export_output_path_pushed_ = false;
    }
    export_output_res_path_ = String();
    default_output_res_path_ = String();
    export_build_failed_ = false;
    export_dedicated_build_enabled_ = false;
}

bool GodotJSExportPlugin::export_raw_file_as(const String& p_source_path, const String& p_export_path, bool p_remap)
{
    if (p_export_path.is_empty() || exported_paths_.has(p_export_path))
    {
        return true;
    }
    Error err;
    const Vector<uint8_t> content = FileAccess::get_file_as_bytes(p_source_path, &err);
    if (err != OK)
    {
        return false;
    }
    exported_paths_.insert(p_export_path);
    add_file(p_export_path, content, p_remap);
    JSB_EXPORTER_LOG(Verbose, "include raw: %s => %s", p_source_path, p_export_path);
    return true;
}

bool GodotJSExportPlugin::export_raw_file(const String& p_path, bool p_remap)
{
    return export_raw_file_as(p_path, p_path, p_remap);
}

bool GodotJSExportPlugin::export_static_asset_file(const String& p_path)
{
    const String normalized_path = normalize_static_asset_path(p_path);
    if (normalized_path.is_empty())
    {
        return false;
    }

    const bool exported = export_raw_file(normalized_path, false);
    if (exported)
    {
        static_asset_paths_.insert(normalized_path);
        JSB_EXPORTER_LOG(Verbose, "include static asset: %s", normalized_path);
    }
    return exported;
}

bool GodotJSExportPlugin::export_static_asset_directory(const String& p_dir)
{
    const String normalized_dir = normalize_static_asset_path(p_dir);
    if (normalized_dir.is_empty())
    {
        return false;
    }

    Ref<DirAccess> dir = DirAccess::open(normalized_dir);
    if (dir.is_null())
    {
        JSB_EXPORTER_LOG(Warning, "Could not open static asset directory for traversal: %s", normalized_dir);
        return false;
    }

    bool exported_any = false;
    dir->set_include_hidden(true);
    dir->list_dir_begin();
    String filename = dir->get_next();
    while (!filename.is_empty())
    {
        if (filename != "." && filename != "..")
        {
            const String path = normalized_dir.path_join(filename);
            if (dir->current_is_dir())
            {
                exported_any = export_static_asset_directory(path) || exported_any;
            }
            else if (filename == ".gdignore")
            {
                JSB_EXPORTER_LOG(Verbose, "skip static asset marker: %s", path);
            }
            else
            {
                exported_any = export_static_asset_file(path) || exported_any;
            }
        }
        filename = dir->get_next();
    }
    dir->list_dir_end();
    return exported_any;
}

bool GodotJSExportPlugin::export_static_asset_path(const String& p_path)
{
    const String normalized_path = normalize_static_asset_path(p_path);
    if (normalized_path.is_empty())
    {
        return false;
    }

    if (FileAccess::exists(normalized_path))
    {
        return export_static_asset_file(normalized_path);
    }

    Ref<DirAccess> dir = DirAccess::open(normalized_path);
    if (dir.is_valid())
    {
        return export_static_asset_directory(normalized_path);
    }

    JSB_EXPORTER_LOG(Warning, "Could not find static asset path: %s", normalized_path);
    return false;
}

void GodotJSExportPlugin::write_web_commonjs_manifest_marker(const String& p_export_path)
{
    if (!is_web_commonjs_export_enabled())
    {
        return;
    }

    const String normalized_path = jsb::internal::WebCommonJSManifest::normalize_path(p_export_path);
    const String marker_path = jsb::internal::WebCommonJSManifest::get_manifest_marker_path(normalized_path);
    if (normalized_path.is_empty() || marker_path.is_empty() || exported_paths_.has(marker_path))
    {
        return;
    }

    Dictionary marker;
    marker["format"] = jsb::internal::WebCommonJSManifest::get_manifest_format();
    marker["version"] = 1;
    marker["path"] = normalized_path;

    Vector<uint8_t> marker_content = JSON::stringify(marker).to_utf8_buffer();
    exported_paths_.insert(marker_path);
    add_file(marker_path, marker_content, false);
    JSB_EXPORTER_LOG(Verbose, "include Web CommonJS manifest marker: %s => %s", normalized_path, marker_path);
}

bool GodotJSExportPlugin::write_external_file_as(const String& p_source_path, const String& p_export_path, bool p_wrap_commonjs)
{
    const String relative_export_path = get_res_relative_path(p_export_path);
    if (relative_export_path.is_empty())
    {
        JSB_EXPORTER_LOG(Warning, "can not export external CommonJS file outside res://: %s", p_export_path);
        return false;
    }
    if (web_commonjs_export_base_dir_.is_empty())
    {
        JSB_EXPORTER_LOG(Warning, "can not export external CommonJS file without an export directory: %s", p_export_path);
        return false;
    }

    const String absolute_export_path = web_commonjs_export_base_dir_.path_join(relative_export_path);
    if (external_paths_.has(absolute_export_path))
    {
        if (is_commonjs_external_file(p_export_path))
        {
            write_web_commonjs_manifest_marker(p_export_path);
        }
        return true;
    }

    Error err;
    const Vector<uint8_t> source = FileAccess::get_file_as_bytes(p_source_path, &err);
    if (err != OK)
    {
        JSB_EXPORTER_LOG(Error, "can't read external CommonJS source from %s", p_source_path);
        return false;
    }

    Vector<uint8_t> content;
    if (p_wrap_commonjs)
    {
        const size_t header_len = sizeof(kCommonJSWrapperHeader) - 1;
        const size_t footer_len = sizeof(kCommonJSWrapperFooter) - 1;
        int source_offset = 0;
        if (source.size() >= 2 && source[0] == '#' && source[1] == '!')
        {
            while (source_offset < source.size() && source[source_offset] != '\n')
            {
                source_offset++;
            }
            if (source_offset < source.size())
            {
                source_offset++;
            }
        }

        const int source_len = source.size() - source_offset;
        content.resize((int) header_len + source_len + (int) footer_len);
        memcpy(content.ptrw(), kCommonJSWrapperHeader, header_len);
        if (source_len > 0)
        {
            memcpy(content.ptrw() + header_len, source.ptr() + source_offset, source_len);
        }
        memcpy(content.ptrw() + header_len + source_len, kCommonJSWrapperFooter, footer_len);
    }
    else
    {
        content = source;
    }

    Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
    if (dir.is_null() || dir->make_dir_recursive(absolute_export_path.get_base_dir()) != OK)
    {
        JSB_EXPORTER_LOG(Error, "can't create external CommonJS export directory for %s", absolute_export_path);
        return false;
    }

    Ref<FileAccess> file = FileAccess::open(absolute_export_path, FileAccess::WRITE, &err);
    if (file.is_null() || err != OK)
    {
        JSB_EXPORTER_LOG(Error, "can't write external CommonJS export file %s", absolute_export_path);
        return false;
    }
    file->store_buffer(content.ptr(), content.size());
    external_paths_.insert(absolute_export_path);
    if (is_commonjs_external_file(p_export_path))
    {
        write_web_commonjs_manifest_marker(p_export_path);
    }
    JSB_EXPORTER_LOG(Verbose, "include external CommonJS: %s => %s", p_source_path, absolute_export_path);
    return true;
}

bool GodotJSExportPlugin::export_external_or_raw_file_as(const String& p_source_path, const String& p_export_path, bool p_remap)
{
    if (is_static_asset_path(p_export_path))
    {
        const bool exported = export_raw_file_as(p_source_path, p_export_path, false);
        if (exported)
        {
            JSB_EXPORTER_LOG(Verbose, "include static asset: %s", p_export_path);
        }
        return exported;
    }

    if (is_web_commonjs_export_enabled() && is_commonjs_external_file(p_export_path))
    {
        const bool is_javascript = is_javascript_like_file(p_export_path);
        const bool exported = write_external_file_as(p_source_path, p_export_path, is_javascript);
        if (exported && is_javascript)
        {
            exported_paths_.insert(p_export_path);
        }
        return exported;
    }
    return export_raw_file_as(p_source_path, p_export_path, p_remap);
}

bool GodotJSExportPlugin::export_external_or_raw_file(const String& p_path, bool p_remap)
{
    return export_external_or_raw_file_as(p_path, p_path, p_remap);
}

void GodotJSExportPlugin::add_remap_file(const String& p_from_path, const String& p_to_path)
{
    const String remap_path = p_from_path + ".remap";
    if (exported_paths_.has(remap_path))
    {
        return;
    }

    const String remap_text = "[remap]\n\npath=\"" + p_to_path.c_escape() + "\"\n";
    Vector<uint8_t> remap_content = remap_text.to_utf8_buffer();
    exported_paths_.insert(remap_path);
    add_file(remap_path, remap_content, false);
    JSB_EXPORTER_LOG(Verbose, "include remap: %s => %s", p_from_path, p_to_path);
}

bool GodotJSExportPlugin::export_directory_as(const String& p_source_dir, const String& p_export_dir)
{
    if (p_source_dir.is_empty() || p_export_dir.is_empty() || !DirAccess::dir_exists_absolute(p_source_dir))
    {
        return false;
    }

    Ref<DirAccess> dir = DirAccess::open(p_source_dir);
    if (dir.is_null())
    {
        return false;
    }

    bool exported_any = false;
    dir->set_include_hidden(true);
    dir->list_dir_begin();
    String filename = dir->get_next();
    while (!filename.is_empty())
    {
        if (filename != "." && filename != "..")
        {
            const String source_path = p_source_dir.path_join(filename);
            const String export_path = p_export_dir.path_join(filename);
            if (dir->current_is_dir())
            {
                exported_any = export_directory_as(source_path, export_path) || exported_any;
            }
            else
            {
                exported_any = export_raw_file_as(source_path, export_path, false) || exported_any;
            }
        }
        filename = dir->get_next();
    }
    dir->list_dir_end();
    return exported_any;
}

bool GodotJSExportPlugin::export_module_files(const jsb::JavaScriptModule& p_module, bool p_remap)
{
    if (!export_external_or_raw_file(p_module.source_info.source_filepath, p_remap))
    {
        JSB_EXPORTER_LOG(Error, "can't read JS source from %s, please ensure that 'tsc' has being executed properly.", p_module.source_info.source_filepath);
        return false;
    }

    if (jsb::internal::Settings::is_packaging_with_source_map())
    {
        const String source_map_path = p_module.source_info.source_filepath + ".map";
        const bool source_map_exported = FileAccess::exists(source_map_path) &&
            (is_web_commonjs_export_enabled()
                ? write_external_file_as(source_map_path, source_map_path, false)
                : export_raw_file(source_map_path, false));
        if (!source_map_exported)
        {
            JSB_EXPORTER_LOG(Verbose, "can't read the sourcemap from %s, please ensure that 'tsc' has being executed properly.", source_map_path);
        }
    }

    if (!p_module.source_info.package_filepath.is_empty())
    {
        if (is_web_commonjs_export_enabled())
        {
            write_external_file_as(p_module.source_info.package_filepath, p_module.source_info.package_filepath, false);
        }
        if (!export_raw_file(p_module.source_info.package_filepath, false))
        {
            JSB_EXPORTER_LOG(Error, "can't read the package.json from %s", p_module.source_info.package_filepath);
            return false;
        }
    }
    return true;
}

bool GodotJSExportPlugin::export_compiled_script(const String& p_path, bool p_remap)
{
    static constexpr char kNodeModulesPrefix[] = "res://node_modules/";

    if (p_path.is_empty() || exported_paths_.has(p_path))
    {
        return false;
    }
    if (!p_path.begins_with("res://"))
    {
        JSB_EXPORTER_LOG(Warning, "can not export external source: %s", p_path);
        return false;
    }
    if (jsb::internal::Settings::is_packaging_referenced_node_modules() && p_path.begins_with(kNodeModulesPrefix))
    {
        // Node modules may dynamically require files within themselves, and thus these modules won't end up in our
        // module's "children" array. The kRtPackagingReferencedNodeModules setting (on by default) allows us to play
        // it safe and export all JS scripts found in referenced packages. However, this won't cover the case where
        // entirely new packages are dynamically imported. kRtPackagingIncludeDirectories must be used to handle that
        // case.
        int package_path_slash_index = p_path.find_char('/', sizeof(kNodeModulesPrefix) - 1);

        if (p_path[sizeof(kNodeModulesPrefix) - 1] == '@' && package_path_slash_index >= 0)
        {
            package_path_slash_index = p_path.find_char('/', package_path_slash_index + 1);
        }

        String package_path = p_path.substr(0, package_path_slash_index);
        Vector<String> script_paths;
        get_script_resources(package_path, script_paths, true);

        const String package_json_path = jsb::internal::PathUtil::combine(package_path, "package.json");

        if (FileAccess::exists(package_json_path))
        {
            script_paths.append(package_json_path);
        }

        export_raw_files(script_paths, false);
        return true;
    }

    // export dependent files.
    // force module loading. ensure the module hierarchy available.
    if (jsb::JavaScriptModule* module; env_->load(p_path, &module) == OK)
    {
        v8::Isolate* isolate = env_->get_isolate();
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);
        const v8::Local<v8::Context> context = env_->get_context();
        v8::Context::Scope context_scope(context);

        if (!export_module_files(*module, p_remap))
        {
            get_export_platform()->add_message(
                EditorExportPlatform::EXPORT_MESSAGE_ERROR,
                get_name(),
                vformat("Failed to export required GodotJS module: %s", p_path));
            return false;
        }
        jsb::Environment* environment = jsb::Environment::wrap(isolate);
        const v8::Local<v8::Object> module_obj = module->module.Get(isolate);
        if (v8::Local<v8::Value> temp; module_obj->Get(context, jsb_name(environment, children)).ToLocal(&temp) && temp->IsArray())
        {
            const v8::Local<v8::Array> children = temp.As<v8::Array>();
            const int32_t len = (int32_t) children->Length();
            for (int i = 0; i < len; i++)
            {
                if (children->Get(context, i).ToLocal(&temp) && temp->IsObject())
                {
                    const v8::Local<v8::Object> child = temp.As<v8::Object>();
                    if (child->Get(context, jsb_name(environment, filename)).ToLocal(&temp))
                    {
                        const String filename = jsb::impl::Helper::to_string(isolate, temp);
                        if (export_compiled_script(filename, false))
                        {
                            JSB_EXPORTER_LOG(Verbose, "export dependent source: %s", filename);
                        }
                    }
                }
            }
        }
    }
    else
    {
        get_export_platform()->add_message(
            EditorExportPlatform::EXPORT_MESSAGE_ERROR,
            get_name(),
            vformat("Failed to load required GodotJS module for export: %s", p_path));
        JSB_EXPORTER_LOG(Warning, "failed to include module: %s", p_path);
        return false;
    }
    return true;
}

#if JSB_WITH_V8
bool GodotJSExportPlugin::generate_and_export_v8_packaged_cache(const String& p_entry_scene)
{
    JSB_EXPORTER_LOG(Verbose, "Preparing V8 packaged code cache from entry scene: %s", p_entry_scene);
    if (p_entry_scene.is_empty() || !p_entry_scene.begins_with("res://"))
    {
        JSB_EXPORTER_LOG(Warning, "Skipping V8 packaged cache generation for invalid entry scene: %s", p_entry_scene);
        return false;
    }

    const String packaged_cache_root = jsb::internal::Settings::get_v8_code_cache_packaged_path();
    JSB_EXPORTER_LOG(Verbose, "V8 packaged code cache export path: %s", packaged_cache_root);
    const String packaged_cache_relative_path = get_res_relative_path(packaged_cache_root);
    if (packaged_cache_relative_path.is_empty())
    {
        JSB_EXPORTER_LOG(Warning, "Skipping V8 packaged cache generation because packaged_cache_path must be a non-empty res:// path: %s", packaged_cache_root);
        return false;
    }

    Error temp_dir_err = OK;
    Ref<DirAccess> temp_project_dir = DirAccess::create_temp("godotjs-v8-cache-", false, &temp_dir_err);
    if (temp_dir_err != OK || temp_project_dir.is_null())
    {
        JSB_EXPORTER_LOG(Warning, "Failed to create temp project mirror for V8 cache export: %s", jsb_ext_error_string(temp_dir_err));
        return false;
    }

    const String source_root = ProjectSettings::get_singleton()->get_resource_path();
    const String mirror_root = temp_project_dir->get_current_dir();
    if (!mirror_project_root(source_root, mirror_root, packaged_cache_root) ||
        !write_v8_export_override_files(source_root, mirror_root, packaged_cache_root))
    {
        JSB_EXPORTER_LOG(Warning, "Failed to prepare temp project mirror for V8 cache export.");
        return false;
    }

    List<String> args;
    args.push_back("--headless");
    args.push_back("--path");
    args.push_back(mirror_root);
    args.push_back("--scene");
    args.push_back(p_entry_scene);

    String helper_output;
    int exit_code = 0;
    const Error execute_err = OS::get_singleton()->execute(OS::get_singleton()->get_executable_path(), args, &helper_output, &exit_code, true);
    if (execute_err != OK || exit_code != 0)
    {
        JSB_EXPORTER_LOG(Warning, "V8 cache export helper failed: err=%s exit_code=%d output=%s", jsb_ext_error_string(execute_err), exit_code, helper_output);
        return false;
    }

    const String generated_cache_dir = mirror_root.path_join(packaged_cache_relative_path);
    const bool exported = export_directory_as(generated_cache_dir, packaged_cache_root);
    if (!exported)
    {
        JSB_EXPORTER_LOG(Warning, "V8 cache export helper ran, but no packaged cache files were found under %s", generated_cache_dir);
    }
    return exported;
}
#endif

void GodotJSExportPlugin::_export_file(const String& p_path, const String& p_type, const HashSet<String>& p_features)
{
    //TODO when exporting for web.impl, need to reorganize all scripts into a monolithic script (like webpack)? and preload it before everything get run.

    if (export_build_failed_)
    {
        skip();
        return;
    }

    if (is_static_asset_path(p_path))
    {
        skip();
        JSB_EXPORTER_LOG(Verbose, "skip static asset already included: %s", p_path);
        return;
    }


    if (p_path.ends_with("." JSB_TYPESCRIPT_EXT))
    {
        const String compiled_script_path = jsb::internal::PathUtil::convert_typescript_path(p_path, export_output_res_path_);
        if (export_dedicated_build_enabled_ && !FileAccess::exists(compiled_script_path))
        {
            skip();
            JSB_EXPORTER_LOG(Verbose, "skip TypeScript source without a compiled Web/export graph entry: %s", p_path);
            return;
        }
        export_compiled_script(compiled_script_path, !is_web_commonjs_export_enabled());

        if (is_web_commonjs_export_enabled())
        {
            add_remap_file(p_path, compiled_script_path);
            skip();
        }

        // always skip the typescript source from packing
        JSB_EXPORTER_LOG(Verbose, "export source: %s => %s", p_path, compiled_script_path);
    }
    else
    {
        if (export_dedicated_build_enabled_ && !web_commonjs_export_ && is_javascript_like_file(p_path)
            && !default_output_res_path_.is_empty()
            && p_path.begins_with(default_output_res_path_ + "/"))
        {
            // A dedicated build owns the selected export graph. Keep
            // legitimate project JavaScript roots and explicit raw assets,
            // while avoiding stale compiler output from the editor's default
            // GodotJS directory; dependency traversal from the selected
            // TypeScript roots adds fresh dedicated modules explicitly.
            skip();
            JSB_EXPORTER_LOG(Verbose, "skip default GodotJS output outside dedicated graph: %s", p_path);
            return;
        }

        if (is_web_commonjs_export_enabled() && is_javascript_like_file(p_path))
        {
            export_compiled_script(p_path, false);
            skip();
            JSB_EXPORTER_LOG(Verbose, "externalized source: %s", p_path);
            return;
        }

        if (ignored_paths_.has(p_path))
        {
            skip();
            JSB_EXPORTER_LOG(Verbose, "ignored: %s", p_path);
        }

        //TODO handle module deps if it's a .js file ?
        // if (p_path.ends_with("." JSB_JAVASCRIPT_EXT))
        // {
        //     export_compiled_script(p_path);
        // }
    }
}

String GodotJSExportPlugin::get_name() const
{
    return jsb_typename(GodotJSExportPlugin);
}

bool GodotJSExportPlugin::supports_platform(const Ref<EditorExportPlatform>& p_export_platform) const
{
    //TODO
    JSB_EXPORTER_LOG(VeryVerbose, "GodotJSExportPlugin::supports_platform( %s )", p_export_platform.is_valid() ? p_export_platform->get_name() : String("null"));
    return true;
}
