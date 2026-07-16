---
"@godot-js/editor": patch
---

**Fix:** Prevent release builds from crashing when JavaScript invokes GDExtension methods. GodotJS now caches their declared types from the release-available `PropertyInfo` metadata instead of reading the GDExtension `MethodBind` cache, which is populated only in debug builds in both Godot 4.6 and 4.7.
