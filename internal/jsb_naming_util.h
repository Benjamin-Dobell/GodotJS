#ifndef GODOTJS_NAMING_UTIL_H
#define GODOTJS_NAMING_UTIL_H

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "jsb_settings.h"

namespace jsb::internal
{
	class NamingUtil
	{
	public:
		static String pascal_to_pascal_case(const String &p_identifier);

		static String snake_to_pascal_case(const String &p_identifier, bool p_input_is_upper = false);

		static String snake_to_camel_case(const String &p_identifier, bool p_input_is_upper = false);

		static List<StringName> get_exposed_original_class_list();

		static bool is_original_class_exposed(const String& p_original_name);

		static String get_class_name(const String& p_original_name)
		{
			if (Settings::get_camel_case_bindings_enabled())
			{
				return pascal_to_pascal_case(p_original_name);
			}

			if (p_original_name == Variant::get_type_name(Variant::DICTIONARY))
			{
				return "GDictionary";
			}

			if (p_original_name == Variant::get_type_name(Variant::ARRAY))
			{
				return "GArray";
			}

			return p_original_name;
		}

		static String get_constant_name(const String& p_original_name)
		{
			return p_original_name;
		}

		static String get_enum_name(const String& p_original_name)
		{
			if (Settings::get_camel_case_bindings_enabled())
			{
				return pascal_to_pascal_case(p_original_name);
			}

			return p_original_name;
		}

		static String get_enum_value_name(const String& p_original_value_name)
		{
			if (Settings::get_camel_case_bindings_enabled())
			{
				return snake_to_pascal_case(p_original_value_name, true);
			}

			return p_original_value_name;
		}

		static String get_member_name(const String& p_original_name)
		{
			if (Settings::get_camel_case_bindings_enabled())
			{
				constexpr int max_cache_entries = 4096;
				thread_local HashMap<String, String> member_name_cache;
				if (const String *cached = member_name_cache.getptr(p_original_name))
				{
					return *cached;
				}

				const String converted = p_original_name.find_char('_') < 0
					? p_original_name
					: snake_to_camel_case(p_original_name);
				if (member_name_cache.size() >= max_cache_entries)
				{
					member_name_cache.clear();
				}
				member_name_cache.insert(p_original_name, converted);
				return converted;
			}

			return p_original_name;
		}

		static String get_parameter_name(const String& p_original_name)
		{
			if (Settings::get_camel_case_bindings_enabled())
			{
				return get_member_name(p_original_name);
			}

			return p_original_name;
		}
	};
}

#endif
