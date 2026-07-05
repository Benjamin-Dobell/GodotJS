#ifndef GODOTJS_EXPORT_PLUGIN_H
#define GODOTJS_EXPORT_PLUGIN_H

#include "jsb_editor_pch.h"

namespace jsb
{
    class Environment;
}

// improve the pipeline of using typescripts
class GodotJSExportPlugin: public EditorExportPlugin
{
    typedef EditorExportPlugin super;
    GDCLASS(GodotJSExportPlugin, EditorExportPlugin)

public:
    GodotJSExportPlugin();
    virtual String get_name() const override;
    virtual bool supports_platform(const Ref<EditorExportPlatform>& p_export_platform) const override;

    static const HashSet<String>& get_ignored_paths() { return ignored_paths_; }

protected:
    virtual void _export_begin(const HashSet<String>& p_features, bool p_debug, const String& p_path, int p_flags) override;
    virtual void _export_file(const String& p_path, const String& p_type, const HashSet<String>& p_features) override;
    virtual void _export_end() override;

    virtual PackedStringArray _get_export_features(const Ref<EditorExportPlatform>& p_export_platform, bool p_debug) const override;
    virtual void _get_export_options(const Ref<EditorExportPlatform>& p_export_platform, List<EditorExportPlatform::ExportOption>* r_options) const override;
    virtual bool _get_export_option_visibility(const Ref<EditorExportPlatform>& p_export_platform, const String& p_option_name) const override;

private:
    static HashSet<String> ignored_paths_;

    static bool is_web_platform(const Ref<EditorExportPlatform>& p_export_platform);
    static bool is_javascript_like_file(const String& p_path);
    static bool is_commonjs_external_file(const String& p_path);
    static String normalize_static_asset_path(const String& p_path);
    static String get_res_relative_path(const String& p_res_path);

    bool is_web_commonjs_export_enabled() const;
    bool is_static_asset_path(const String& p_path) const;
    void write_web_commonjs_manifest_marker(const String& p_export_path);
    bool write_external_file_as(const String& p_source_path, const String& p_export_path, bool p_wrap_commonjs);
    bool export_external_or_raw_file_as(const String& p_source_path, const String& p_export_path, bool p_remap);
    bool export_external_or_raw_file(const String& p_path, bool p_remap);
    void add_remap_file(const String& p_from_path, const String& p_to_path);
    bool export_compiled_script(const String& p_path, bool p_remap);
    bool export_module_files(const jsb::JavaScriptModule& p_module, bool p_remap);
    bool export_raw_file_as(const String& p_source_path, const String& p_export_path, bool p_remap);
    bool export_raw_file(const String& p_path, bool p_remap);
    bool export_directory_as(const String& p_source_dir, const String& p_export_dir);
    bool export_static_asset_file(const String& p_path);
    bool export_static_asset_directory(const String& p_dir);
    bool export_static_asset_path(const String& p_path);
    void export_raw_files(const PackedStringArray& p_paths, bool p_permit_typescript);
    void get_script_resources(const String &p_dir, Vector<String> &r_list, bool p_is_node_module = false);
#if JSB_WITH_V8
    bool generate_and_export_v8_packaged_cache(const String& p_entry_scene);
#endif

    HashSet<String> exported_paths_;
    HashSet<String> external_paths_;
    HashSet<String> static_asset_paths_;
    bool web_commonjs_export_ = false;
    String web_commonjs_export_base_dir_;
    std::shared_ptr<jsb::Environment> env_;
};

#endif
