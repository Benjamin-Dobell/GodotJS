#include "jsb_web_commonjs_manifest.h"

#include "jsb_internal_pch.h"
#include "jsb_logger.h"

#include "core/io/json.h"

namespace jsb::internal
{
    namespace
    {
        static constexpr char kManifestMarkerDir[] = "res://.godot/GodotJS/web_commonjs_manifest";
        static constexpr char kManifestFormat[] = "godotjs-web-commonjs";
#if JSB_WITH_WEB
        static constexpr char kManifestFormatKey[] = "format";
        static constexpr char kManifestPathKey[] = "path";

        bool has_marker_path(const String& p_normalized_path)
        {
            const String marker_path = WebCommonJSManifest::get_manifest_marker_path(p_normalized_path);
            if (marker_path.is_empty())
            {
                return false;
            }

            Error err = OK;
            const String marker_text = FileAccess::get_file_as_string(marker_path, &err);
            if (err != OK)
            {
                return false;
            }

            const Variant parsed = JSON::parse_string(marker_text);
            if (parsed.get_type() != Variant::DICTIONARY)
            {
                JSB_LOG(Warning, "invalid Web CommonJS manifest marker: %s", marker_path);
                return false;
            }

            const Dictionary marker = parsed;
            if ((String) marker.get(kManifestFormatKey, String()) != kManifestFormat)
            {
                JSB_LOG(Warning, "invalid Web CommonJS manifest marker format: %s", marker_path);
                return false;
            }

            return WebCommonJSManifest::normalize_path((String) marker.get(kManifestPathKey, String())) == p_normalized_path;
        }
#endif
    }

    String WebCommonJSManifest::get_manifest_marker_path(const String& p_path)
    {
        const String normalized_path = normalize_path(p_path);
        if (normalized_path.is_empty())
        {
            return String();
        }
        return String(kManifestMarkerDir).path_join(normalized_path.md5_text() + ".json");
    }

    String WebCommonJSManifest::get_manifest_format()
    {
        return kManifestFormat;
    }

    String WebCommonJSManifest::normalize_path(const String& p_path)
    {
        String path = p_path.replace("\\", "/");
        if (path.begins_with("res:/") && !path.begins_with("res://"))
        {
            path = "res://" + path.substr(5);
        }
        if (!path.begins_with("res://"))
        {
            return String();
        }

        const String relative_path = path.substr(6).simplify_path();
        if (relative_path.is_empty() || relative_path == ".")
        {
            return String();
        }
        return "res://" + relative_path;
    }

    bool WebCommonJSManifest::has_path(const String& p_path)
    {
#if JSB_WITH_WEB
        const String normalized_path = normalize_path(p_path);
        if (normalized_path.is_empty())
        {
            return false;
        }

        return has_marker_path(normalized_path);
#else
        return false;
#endif
    }
}
