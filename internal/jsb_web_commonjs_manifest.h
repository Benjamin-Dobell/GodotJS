#ifndef GODOTJS_WEB_COMMONJS_MANIFEST_H
#define GODOTJS_WEB_COMMONJS_MANIFEST_H

#include "../compat/jsb_compat.h"

namespace jsb::internal
{
    class WebCommonJSManifest
    {
    public:
        static String get_manifest_marker_path(const String& p_path);
        static String get_manifest_format();
        static String normalize_path(const String& p_path);
        static bool has_path(const String& p_path);
    };
}

#endif
