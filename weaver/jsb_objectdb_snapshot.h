#ifndef GODOTJS_OBJECTDB_SNAPSHOT_H
#define GODOTJS_OBJECTDB_SNAPSHOT_H

#include "../compat/jsb_compat.h"

class GodotJSObjectDBSnapshot : public Object
{
	GDCLASS(GodotJSObjectDBSnapshot, Object)

protected:
	static void _bind_methods();

public:
	static Dictionary capture_to_file(const String &p_snapshot_file_path, const Dictionary &p_snapshot_context = Dictionary());
	static Dictionary capture_bridge_snapshot(bool p_include_live_entries = true, int p_max_entries = 0);
	static Dictionary capture_bridge_snapshot_to_file(const String &p_report_file_path, bool p_include_live_entries = true, int p_max_entries = 0);
};

#endif
