#include "jsb_objectdb_snapshot.h"

#include "core/config/project_settings.h"
#include "core/core_bind.h"
#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "../bridge/jsb_environment.h"
#include "modules/objectdb_profiler/snapshot_collector.h"

void GodotJSObjectDBSnapshot::_bind_methods()
{
	ClassDB::bind_static_method(
			jsb_typename(GodotJSObjectDBSnapshot),
			D_METHOD("capture_pixel_memory_snapshot", "include_allocator"),
			&GodotJSObjectDBSnapshot::capture_pixel_memory_snapshot,
			DEFVAL(false));
	ClassDB::bind_static_method(
			jsb_typename(GodotJSObjectDBSnapshot),
			D_METHOD("capture_to_file", "snapshot_file_path", "snapshot_context"),
			&GodotJSObjectDBSnapshot::capture_to_file,
			DEFVAL(Dictionary()));
	ClassDB::bind_static_method(
			jsb_typename(GodotJSObjectDBSnapshot),
			D_METHOD("capture_bridge_snapshot", "include_live_entries", "max_entries"),
			&GodotJSObjectDBSnapshot::capture_bridge_snapshot,
			DEFVAL(true),
			DEFVAL(0));
	ClassDB::bind_static_method(
			jsb_typename(GodotJSObjectDBSnapshot),
			D_METHOD("capture_bridge_snapshot_to_file", "report_file_path", "include_live_entries", "max_entries"),
			&GodotJSObjectDBSnapshot::capture_bridge_snapshot_to_file,
			DEFVAL(true),
			DEFVAL(0));
}

Dictionary GodotJSObjectDBSnapshot::capture_pixel_memory_snapshot(bool p_include_allocator)
{
	const std::shared_ptr<jsb::Environment> env = jsb::Environment::_access();
	ERR_FAIL_COND_V_MSG(!env, Dictionary(), "No active GodotJS Environment for current thread/isolate.");
	return env->get_pixel_memory_snapshot(p_include_allocator);
}

Dictionary GodotJSObjectDBSnapshot::capture_to_file(const String &p_snapshot_file_path, const Dictionary &p_snapshot_context)
{
	Dictionary result;
	result["ok"] = false;

#ifdef DEBUG_ENABLED
	ERR_FAIL_COND_V_MSG(p_snapshot_file_path.is_empty(), result, "capture_to_file requires a non-empty snapshot_file_path");

	String output_path = p_snapshot_file_path.contains("://")
			? p_snapshot_file_path
			: ProjectSettings::get_singleton()->globalize_path(p_snapshot_file_path);
	const Error mkdir_err = DirAccess::make_dir_recursive_absolute(output_path.get_base_dir());
	ERR_FAIL_COND_V_MSG(mkdir_err != OK, result, "Failed creating snapshot directory: " + output_path.get_base_dir());

	Dictionary snapshot_context = p_snapshot_context;
	Array objects;
	SnapshotCollector::snapshot_objects(&objects, snapshot_context);

	CoreBind::Marshalls *m = CoreBind::Marshalls::get_singleton();
	Vector<uint8_t> raw = m->base64_to_raw(m->variant_to_base64(objects));

	Vector<uint8_t> compressed;
	compressed.resize(raw.size());
	const int64_t compressed_size = Compression::compress(
			compressed.ptrw(),
			raw.ptr(),
			raw.size(),
			Compression::MODE_DEFLATE);
	ERR_FAIL_COND_V_MSG(compressed_size < 0, result, "Failed compressing ObjectDB snapshot payload");
	compressed.resize(compressed_size);

	Error open_err = OK;
	Ref<FileAccess> file = FileAccess::open(output_path, FileAccess::WRITE, &open_err);
	ERR_FAIL_COND_V_MSG(open_err != OK || file.is_null(), result, "Failed opening snapshot file for write: " + output_path);
	file->store_buffer(compressed);
	file->close();

	const int64_t object_count = objects.size() > 1 ? (objects.size() - 1) / 2 : 0;

	result["ok"] = true;
	result["path"] = output_path;
	result["object_count"] = object_count;
	result["raw_size"] = raw.size();
	result["compressed_size"] = compressed_size;
	result["serialized_entries"] = objects.size();
	return result;
#else
	result["error"] = "ObjectDB snapshots require a DEBUG_ENABLED build.";
	return result;
#endif
}

Dictionary GodotJSObjectDBSnapshot::capture_bridge_snapshot(bool p_include_live_entries, int p_max_entries)
{
	Dictionary result;
	result["ok"] = false;

	const std::shared_ptr<jsb::Environment> env = jsb::Environment::_access();
	ERR_FAIL_COND_V_MSG(!env, result, "No active GodotJS Environment for current thread/isolate.");

	result = env->get_object_origin_snapshot(p_include_live_entries, p_max_entries);
	const bool debug_enabled = (bool) result.get("debug_enabled", true);
	result["ok"] = debug_enabled;
	return result;
}

Dictionary GodotJSObjectDBSnapshot::capture_bridge_snapshot_to_file(const String &p_report_file_path, bool p_include_live_entries, int p_max_entries)
{
	Dictionary result = capture_bridge_snapshot(p_include_live_entries, p_max_entries);
	if (!(bool)result.get("ok", false))
	{
		return result;
	}

	ERR_FAIL_COND_V_MSG(p_report_file_path.is_empty(), result, "capture_bridge_snapshot_to_file requires a non-empty report_file_path");
	String output_path = p_report_file_path.contains("://")
			? p_report_file_path
			: ProjectSettings::get_singleton()->globalize_path(p_report_file_path);
	const Error mkdir_err = DirAccess::make_dir_recursive_absolute(output_path.get_base_dir());
	ERR_FAIL_COND_V_MSG(mkdir_err != OK, result, "Failed creating report directory: " + output_path.get_base_dir());

	Error open_err = OK;
	Ref<FileAccess> file = FileAccess::open(output_path, FileAccess::WRITE, &open_err);
	ERR_FAIL_COND_V_MSG(open_err != OK || file.is_null(), result, "Failed opening report file for write: " + output_path);

	file->store_string(JSON::stringify(result, "  "));
	file->store_string("\n");
	file->close();
	result["path"] = output_path;
	return result;
}
