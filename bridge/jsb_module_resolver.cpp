#include "jsb_module_resolver.h"
#include "jsb_environment.h"

#include "../internal/jsb_path_util.h"
#include "../internal/jsb_web_commonjs_manifest.h"

#if JSB_WITH_V8
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"
#endif

namespace jsb
{
    namespace
    {
        // the cost of copy return is acceptable since Vector copy constructor is by-reference under the hood
        PackedStringArray get_dynamic_search_paths()
        {
#ifdef TOOLS_ENABLED
            return jsb::internal::Settings::get_additional_search_paths();
#else
            static PackedStringArray dynamic_search_paths = jsb::internal::Settings::get_additional_search_paths();
            return dynamic_search_paths;
#endif
        }

#if JSB_WITH_WEB
        static constexpr char kGodotJSHostRequire[] = "__godotjs_host_require";

        bool get_web_host_require(Environment* p_env, v8::Local<v8::Function>& r_require)
        {
            if (!p_env)
            {
                return false;
            }

            v8::Isolate* isolate = p_env->get_isolate();
            const v8::Local<v8::Context> context = p_env->get_context();
            v8::Local<v8::Value> require_val;
            if (!context->Global()->Get(context, impl::Helper::new_string_ascii(isolate, kGodotJSHostRequire)).ToLocal(&require_val) ||
                !require_val->IsFunction())
            {
                return false;
            }

            r_require = require_val.As<v8::Function>();
            return true;
        }

        bool has_web_host_require(Environment* p_env)
        {
            v8::Local<v8::Function> require_func;
            return get_web_host_require(p_env, require_func);
        }

        bool is_web_host_require_candidate(Environment* p_env, const String& p_path)
        {
            return has_web_host_require(p_env) &&
                internal::WebCommonJSManifest::has_path(p_path) &&
                (internal::PathUtil::is_recognized_javascript_extension(p_path) || p_path.ends_with("." JSB_JSON_EXT));
        }

        String to_web_host_require_id(const String& p_asset_path)
        {
            return p_asset_path.begins_with("res://")
                ? "./" + p_asset_path.substr(6)
                : p_asset_path;
        }

        bool load_with_web_host_require(Environment* p_env, JavaScriptModule& p_module, const String& p_asset_path)
        {
            v8::Local<v8::Function> require_func;
            if (!get_web_host_require(p_env, require_func))
            {
                return false;
            }

            v8::Isolate* isolate = p_env->get_isolate();
            const v8::Local<v8::Context> context = p_env->get_context();
            const String require_id = to_web_host_require_id(p_asset_path);
            v8::Local<v8::Value> argv[] = {
                impl::Helper::new_string(isolate, require_id),
            };

            v8::Local<v8::Value> loaded_value;
            if (!require_func->Call(context, v8::Undefined(isolate), 1, argv).ToLocal(&loaded_value))
            {
                return false;
            }

            if (loaded_value->IsFunction())
            {
                return IModuleResolver::load_from_evaluator(p_env, p_module, p_asset_path, loaded_value.As<v8::Function>());
            }

            const String dirname = internal::PathUtil::dirname(p_asset_path);
            const v8::Local<v8::Object> module_obj = p_module.module.Get(isolate);
            module_obj->Set(context, jsb_name(p_env, filename), impl::Helper::new_string(isolate, p_asset_path)).Check();
            module_obj->Set(context, jsb_name(p_env, path), impl::Helper::new_string(isolate, dirname)).Check();
            module_obj->Set(context, jsb_name(p_env, exports), loaded_value).Check();
            p_module.exports.Reset(isolate, loaded_value);
            return true;
        }
#endif

#if JSB_WITH_V8
        static constexpr char kV8CodeCacheMagic[] = "JSBV8CC1";
        static constexpr char kV8CodeCacheCompileMode[] = "cjsw1";
        static constexpr uint32_t kV8CodeCacheFileVersion = 1;
        static constexpr uint32_t kV8CodeCacheMaxPayloadBytes = 8 * 1024 * 1024;
        static constexpr uint32_t kV8CodeCacheEnvelopeBytes =
            (uint32_t) (sizeof(kV8CodeCacheMagic) - 1 + sizeof(uint32_t) + sizeof(kV8CodeCacheCompileMode) - 1 + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) + (32 * 3));

        enum class V8CodeCacheProvider
        {
            None,
            Packaged,
            UnsafeUser,
        };

        struct V8CodeCacheIdentity
        {
            uint8_t source_hash[32] = {};
            uint8_t origin_hash[32] = {};
            String source_hash_hex;
            String origin_hash_hex;
            uint64_t source_length = 0;
            uint32_t cached_data_version_tag = 0;
            String relative_path;
        };

        struct V8CodeCacheLoadResult
        {
            String path;
            Vector<uint8_t> cache_bytes;
            V8CodeCacheProvider provider = V8CodeCacheProvider::None;
            bool delete_on_reject = false;
            bool hit = false;
        };

        struct V8CodeCacheState
        {
            V8CodeCacheIdentity identity;
            V8CodeCacheLoadResult loaded;
            String packaged_path;
            String write_path;
            bool write_enabled = false;
        };

        bool should_log_v8_code_cache()
        {
            return jsb::internal::Settings::is_v8_code_cache_logging_enabled();
        }

        bool compute_sha256_bytes(const uint8_t* p_source, size_t p_source_len, uint8_t (&r_hash)[32])
        {
            return CryptoCore::sha256(p_source, p_source_len, r_hash) == OK;
        }

        bool compute_sha256_string(const String& p_source, uint8_t (&r_hash)[32])
        {
            const CharString utf8 = p_source.utf8();
            return compute_sha256_bytes((const uint8_t*) utf8.ptr(), utf8.length(), r_hash);
        }

        String get_sha256_hex(const uint8_t (&p_hash)[32])
        {
            return String::hex_encode_buffer(p_hash, 32);
        }

        void append_uint32(Vector<uint8_t>& r_bytes, uint32_t p_value)
        {
            const int offset = r_bytes.size();
            r_bytes.resize(offset + 4);
            encode_uint32(p_value, r_bytes.ptrw() + offset);
        }

        void append_uint64(Vector<uint8_t>& r_bytes, uint64_t p_value)
        {
            const int offset = r_bytes.size();
            r_bytes.resize(offset + 8);
            encode_uint64(p_value, r_bytes.ptrw() + offset);
        }

        void append_raw_bytes(Vector<uint8_t>& r_bytes, const uint8_t* p_data, int p_length)
        {
            const int offset = r_bytes.size();
            r_bytes.resize(offset + p_length);
            memcpy(r_bytes.ptrw() + offset, p_data, p_length);
        }

        bool read_uint32(const Vector<uint8_t>& p_bytes, int& r_offset, uint32_t& r_value)
        {
            if (r_offset + 4 > p_bytes.size())
            {
                return false;
            }
            r_value = decode_uint32(p_bytes.ptr() + r_offset);
            r_offset += 4;
            return true;
        }

        bool read_uint64(const Vector<uint8_t>& p_bytes, int& r_offset, uint64_t& r_value)
        {
            if (r_offset + 8 > p_bytes.size())
            {
                return false;
            }
            r_value = decode_uint64(p_bytes.ptr() + r_offset);
            r_offset += 8;
            return true;
        }

        bool read_fixed_bytes(const Vector<uint8_t>& p_bytes, int& r_offset, uint8_t* r_target, int p_length)
        {
            if (r_offset + p_length > p_bytes.size())
            {
                return false;
            }
            memcpy(r_target, p_bytes.ptr() + r_offset, p_length);
            r_offset += p_length;
            return true;
        }

        String make_v8_code_cache_filename_prefix(const V8CodeCacheIdentity& p_identity)
        {
            const String version_tag = String::num_uint64(p_identity.cached_data_version_tag, 16);
            const String source_len = String::num_uint64(p_identity.source_length, 16);
            return vformat(
                "%s-%s-%s-%s-",
                String(kV8CodeCacheCompileMode),
                version_tag,
                source_len,
                p_identity.source_hash_hex);
        }

        String make_v8_code_cache_relative_path(const V8CodeCacheIdentity& p_identity)
        {
            const String shard = p_identity.source_hash_hex.substr(0, 2);
            const String filename = vformat(
                "%s%s.v8code",
                make_v8_code_cache_filename_prefix(p_identity),
                p_identity.origin_hash_hex.substr(0, 16));
            return shard.path_join(filename);
        }

        bool make_v8_code_cache_identity(const String& p_source_url, const uint8_t* p_source, size_t p_source_len, V8CodeCacheIdentity& r_identity)
        {
            if (!compute_sha256_bytes(p_source, p_source_len, r_identity.source_hash))
            {
                return false;
            }

            if (!compute_sha256_string(impl::Helper::get_script_origin_identity(p_source_url), r_identity.origin_hash))
            {
                return false;
            }

            r_identity.source_hash_hex = get_sha256_hex(r_identity.source_hash);
            r_identity.origin_hash_hex = get_sha256_hex(r_identity.origin_hash);
            r_identity.source_length = (uint64_t) p_source_len;
            r_identity.cached_data_version_tag = v8::ScriptCompiler::CachedDataVersionTag();
            r_identity.relative_path = make_v8_code_cache_relative_path(r_identity);
            return true;
        }

        String make_v8_code_cache_path(const String& p_cache_root, const V8CodeCacheIdentity& p_identity)
        {
            if (p_cache_root.is_empty() || p_identity.relative_path.is_empty())
            {
                return String();
            }
            return p_cache_root.path_join(p_identity.relative_path);
        }

        bool remove_v8_code_cache_file(const String& p_path)
        {
            Ref<DirAccess> dir = DirAccess::create_for_path(p_path);
            if (dir.is_null())
            {
                return false;
            }
            return dir->remove(p_path) == OK;
        }

        Vector<uint8_t> encode_v8_code_cache_file(const V8CodeCacheIdentity& p_identity, const Vector<uint8_t>& p_cache_bytes)
        {
            uint8_t payload_hash[32];
            Vector<uint8_t> encoded;
            if (!compute_sha256_bytes(p_cache_bytes.ptr(), p_cache_bytes.size(), payload_hash))
            {
                return encoded;
            }

            encoded.reserve((int) (kV8CodeCacheEnvelopeBytes + p_cache_bytes.size()));
            append_raw_bytes(encoded, (const uint8_t*) kV8CodeCacheMagic, sizeof(kV8CodeCacheMagic) - 1);
            append_uint32(encoded, kV8CodeCacheFileVersion);
            append_raw_bytes(encoded, (const uint8_t*) kV8CodeCacheCompileMode, sizeof(kV8CodeCacheCompileMode) - 1);
            append_uint32(encoded, p_identity.cached_data_version_tag);
            append_uint64(encoded, p_identity.source_length);
            append_uint32(encoded, (uint32_t) p_cache_bytes.size());
            append_raw_bytes(encoded, p_identity.source_hash, 32);
            append_raw_bytes(encoded, p_identity.origin_hash, 32);
            append_raw_bytes(encoded, payload_hash, 32);
            append_raw_bytes(encoded, p_cache_bytes.ptr(), p_cache_bytes.size());
            return encoded;
        }

        bool decode_v8_code_cache_payload(
            const String& p_path,
            const Vector<uint8_t>& p_file_bytes,
            const V8CodeCacheIdentity& p_identity,
            Vector<uint8_t>& r_payload,
            String& r_reason,
            bool p_allow_origin_mismatch = false,
            bool* r_origin_mismatch = nullptr)
        {
            if (r_origin_mismatch)
            {
                *r_origin_mismatch = false;
            }

            if (p_file_bytes.size() < (int) kV8CodeCacheEnvelopeBytes)
            {
                r_reason = "file too small";
                return false;
            }

            int offset = 0;
            uint8_t magic[sizeof(kV8CodeCacheMagic) - 1];
            if (!read_fixed_bytes(p_file_bytes, offset, magic, sizeof(magic)) || memcmp(magic, kV8CodeCacheMagic, sizeof(magic)) != 0)
            {
                r_reason = "bad magic";
                return false;
            }

            uint32_t file_version = 0;
            if (!read_uint32(p_file_bytes, offset, file_version) || file_version != kV8CodeCacheFileVersion)
            {
                r_reason = "unsupported file version";
                return false;
            }

            uint8_t compile_mode[sizeof(kV8CodeCacheCompileMode) - 1];
            if (!read_fixed_bytes(p_file_bytes, offset, compile_mode, sizeof(compile_mode)) || memcmp(compile_mode, kV8CodeCacheCompileMode, sizeof(compile_mode)) != 0)
            {
                r_reason = "unsupported compile mode";
                return false;
            }

            uint32_t cached_data_version_tag = 0;
            uint64_t source_length = 0;
            uint32_t payload_length = 0;
            uint8_t source_hash[32];
            uint8_t origin_hash[32];
            uint8_t payload_hash[32];
            if (!read_uint32(p_file_bytes, offset, cached_data_version_tag) ||
                !read_uint64(p_file_bytes, offset, source_length) ||
                !read_uint32(p_file_bytes, offset, payload_length) ||
                !read_fixed_bytes(p_file_bytes, offset, source_hash, 32) ||
                !read_fixed_bytes(p_file_bytes, offset, origin_hash, 32) ||
                !read_fixed_bytes(p_file_bytes, offset, payload_hash, 32))
            {
                r_reason = "truncated metadata";
                return false;
            }

            if (cached_data_version_tag != p_identity.cached_data_version_tag)
            {
                r_reason = "V8 cache version mismatch";
                return false;
            }
            if (source_length != p_identity.source_length)
            {
                r_reason = "wrapped source length mismatch";
                return false;
            }
            if (memcmp(source_hash, p_identity.source_hash, 32) != 0)
            {
                r_reason = "wrapped source hash mismatch";
                return false;
            }
            if (memcmp(origin_hash, p_identity.origin_hash, 32) != 0)
            {
                if (!p_allow_origin_mismatch)
                {
                    r_reason = "script origin hash mismatch";
                    return false;
                }
                if (r_origin_mismatch)
                {
                    *r_origin_mismatch = true;
                }
            }
            if (payload_length == 0 || payload_length > kV8CodeCacheMaxPayloadBytes)
            {
                r_reason = "payload size out of bounds";
                return false;
            }
            if ((uint64_t) offset + payload_length != (uint64_t) p_file_bytes.size())
            {
                r_reason = "payload length mismatch";
                return false;
            }

            uint8_t computed_payload_hash[32];
            if (!compute_sha256_bytes(p_file_bytes.ptr() + offset, payload_length, computed_payload_hash) || memcmp(payload_hash, computed_payload_hash, 32) != 0)
            {
                r_reason = "payload hash mismatch";
                return false;
            }

            r_payload.resize(payload_length);
            memcpy(r_payload.ptrw(), p_file_bytes.ptr() + offset, payload_length);
            jsb_unused(p_path);
            return true;
        }

        bool read_v8_code_cache_file(const String& p_path, Vector<uint8_t>& r_file_bytes, String& r_reason)
        {
            Error open_err = OK;
            Ref<FileAccess> cache_file = FileAccess::open(p_path, FileAccess::READ, &open_err);
            if (open_err != OK || cache_file.is_null())
            {
                r_reason = jsb_ext_error_string(open_err);
                return false;
            }

            const uint64_t file_length = cache_file->get_length();
            if (file_length < kV8CodeCacheEnvelopeBytes)
            {
                r_reason = "file too small";
                return false;
            }
            if (file_length > kV8CodeCacheEnvelopeBytes + kV8CodeCacheMaxPayloadBytes)
            {
                r_reason = "file exceeds max cache size";
                return false;
            }

            r_file_bytes.resize((int) file_length);
            if (cache_file->get_buffer(r_file_bytes.ptrw(), (uint64_t) r_file_bytes.size()) != (uint64_t) r_file_bytes.size())
            {
                r_file_bytes.clear();
                r_reason = "short read";
                return false;
            }

            return true;
        }

        bool try_load_v8_code_cache(const String& p_path, V8CodeCacheProvider p_provider, bool p_delete_on_invalid, const V8CodeCacheIdentity& p_identity, V8CodeCacheLoadResult& r_result, bool p_allow_origin_mismatch = false)
        {
            if (p_path.is_empty() || !FileAccess::exists(p_path))
            {
                return false;
            }

            Vector<uint8_t> file_bytes;
            String reason;
            bool origin_mismatch = false;
            if (!read_v8_code_cache_file(p_path, file_bytes, reason) ||
                !decode_v8_code_cache_payload(p_path, file_bytes, p_identity, r_result.cache_bytes, reason, p_allow_origin_mismatch, &origin_mismatch))
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "rejecting V8 code cache before V8: %s (%s)", p_path, reason);
                }
                if (p_delete_on_invalid)
                {
                    remove_v8_code_cache_file(p_path);
                }
                return false;
            }

            r_result.path = p_path;
            r_result.provider = p_provider;
            r_result.delete_on_reject = p_delete_on_invalid;
            r_result.hit = true;
            if (should_log_v8_code_cache())
            {
                JSB_LOG(Verbose, "V8 code cache hit: %s", p_path);
                if (origin_mismatch)
                {
                    JSB_LOG(Verbose, "V8 code cache accepted with packaged origin mismatch: %s", p_path);
                }
            }
            return true;
        }

        bool try_load_packaged_v8_code_cache_by_source(const String& p_cache_root, const V8CodeCacheIdentity& p_identity, V8CodeCacheLoadResult& r_result)
        {
            if (p_cache_root.is_empty() || p_identity.source_hash_hex.length() < 2)
            {
                return false;
            }

            const String shard_dir = p_cache_root.path_join(p_identity.source_hash_hex.substr(0, 2));
            Ref<DirAccess> dir = DirAccess::open(shard_dir);
            if (dir.is_null())
            {
                return false;
            }

            const String filename_prefix = make_v8_code_cache_filename_prefix(p_identity);
            dir->set_include_hidden(true);
            dir->list_dir_begin();
            String filename = dir->get_next();
            while (!filename.is_empty())
            {
                if (!dir->current_is_dir() && filename.begins_with(filename_prefix) && filename.ends_with(".v8code"))
                {
                    const String candidate_path = shard_dir.path_join(filename);
                    if (try_load_v8_code_cache(candidate_path, V8CodeCacheProvider::Packaged, false, p_identity, r_result, true))
                    {
                        if (should_log_v8_code_cache())
                        {
                            JSB_LOG(Verbose, "V8 packaged code cache source-hash fallback hit: %s", candidate_path);
                        }
                        dir->list_dir_end();
                        return true;
                    }
                }
                filename = dir->get_next();
            }
            dir->list_dir_end();
            return false;
        }

        V8CodeCacheState load_v8_code_cache(const String& p_source_url, const uint8_t* p_source, size_t p_source_len)
        {
            V8CodeCacheState state;
            if (!make_v8_code_cache_identity(p_source_url, p_source, p_source_len, state.identity))
            {
                return state;
            }

            const String packaged_cache_root = jsb::internal::Settings::get_v8_code_cache_packaged_path();
            state.packaged_path = make_v8_code_cache_path(packaged_cache_root, state.identity);
            if (!state.packaged_path.is_empty())
            {
                try_load_v8_code_cache(state.packaged_path, V8CodeCacheProvider::Packaged, false, state.identity, state.loaded);
                if (!state.loaded.hit && should_log_v8_code_cache())
                {
                    JSB_LOG(Verbose, "V8 packaged code cache miss: %s", state.packaged_path);
                }
                if (!state.loaded.hit)
                {
                    try_load_packaged_v8_code_cache_by_source(packaged_cache_root, state.identity, state.loaded);
                }
            }

            const bool unsafe_user_enabled = jsb::internal::Settings::get_v8_code_cache_unsafe_user_enabled();
            if (!state.loaded.hit && unsafe_user_enabled)
            {
                const String unsafe_user_path = make_v8_code_cache_path(jsb::internal::Settings::get_v8_code_cache_unsafe_user_path(), state.identity);
                try_load_v8_code_cache(unsafe_user_path, V8CodeCacheProvider::UnsafeUser, true, state.identity, state.loaded);
                state.write_enabled = !unsafe_user_path.is_empty();
                state.write_path = unsafe_user_path;
                if (state.write_enabled && should_log_v8_code_cache())
                {
                    JSB_LOG(Verbose, "V8 unsafe user cache write target: %s", state.write_path);
                }
            }

            if (jsb::internal::Settings::is_v8_code_cache_packaged_generation_enabled())
            {
                state.write_enabled = !state.packaged_path.is_empty();
                state.write_path = state.packaged_path;
                if (state.write_enabled && should_log_v8_code_cache())
                {
                    JSB_LOG(Verbose, "V8 packaged cache write target: %s", state.write_path);
                }
            }

            return state;
        }

        void store_v8_code_cache(const V8CodeCacheState& p_state, const Vector<uint8_t>& p_cache_bytes)
        {
            if (!p_state.write_enabled || p_state.write_path.is_empty() || p_cache_bytes.is_empty())
            {
                if (p_state.write_enabled && p_cache_bytes.is_empty() && should_log_v8_code_cache())
                {
                    JSB_LOG(Verbose, "V8 did not produce code cache bytes for: %s", p_state.write_path);
                }
                return;
            }

            Vector<uint8_t> encoded_bytes = encode_v8_code_cache_file(p_state.identity, p_cache_bytes);
            if (encoded_bytes.is_empty())
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "failed to encode V8 code cache envelope: %s", p_state.write_path);
                }
                return;
            }

            const String cache_dir = p_state.write_path.get_base_dir();
            Ref<DirAccess> dir = DirAccess::create_for_path(cache_dir);
            if (dir.is_null())
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "failed to create V8 code cache directory access: %s", cache_dir);
                }
                return;
            }

            const Error make_dir_err = dir->make_dir_recursive(cache_dir);
            if (make_dir_err != OK)
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "failed to create V8 code cache directory: %s (%s)", cache_dir, jsb_ext_error_string(make_dir_err));
                }
                return;
            }

            const String temp_path = vformat("%s.tmp.%s.%s", p_state.write_path, itos(OS::get_singleton()->get_process_id()), itos(OS::get_singleton()->get_ticks_usec()));
            Error open_err = OK;
            Ref<FileAccess> cache_file = FileAccess::open(temp_path, FileAccess::WRITE, &open_err);
            if (open_err != OK || cache_file.is_null())
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "failed to open temporary V8 code cache for write: %s (%s)", temp_path, jsb_ext_error_string(open_err));
                }
                return;
            }

            if (!cache_file->store_buffer(encoded_bytes.ptr(), encoded_bytes.size()))
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "failed to write temporary V8 code cache: %s", temp_path);
                }
                dir->remove(temp_path);
                return;
            }

            cache_file.unref();
            if (FileAccess::exists(p_state.write_path))
            {
                dir->remove(p_state.write_path);
            }
            const Error rename_err = dir->rename(temp_path, p_state.write_path);
            if (rename_err != OK)
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "failed to finalize V8 code cache write: %s (%s)", p_state.write_path, jsb_ext_error_string(rename_err));
                }
                dir->remove(temp_path);
                return;
            }

            if (should_log_v8_code_cache())
            {
                JSB_LOG(Verbose, "stored V8 code cache: %s (%s bytes)", p_state.write_path, String::num_uint64((uint64_t) encoded_bytes.size()));
            }
        }
#endif
    }

    bool IModuleResolver::load_as_json(Environment* p_env, JavaScriptModule& p_module, const String& p_asset_path, const Vector<uint8_t>& p_bytes, size_t p_len)
    {
        v8::Isolate* isolate = p_env->get_isolate();
        const v8::Local<v8::Context> context = isolate->GetCurrentContext();
        const v8::Local<v8::Object> module_obj = p_module.module.Get(isolate);
        const String& filename = p_asset_path;
        const String dirname = internal::PathUtil::dirname(filename);

        module_obj->Set(context, jsb_name(p_env, filename), impl::Helper::new_string(isolate, filename)).Check();
        module_obj->Set(context, jsb_name(p_env, path), impl::Helper::new_string(isolate, dirname)).Check();

        v8::Local<v8::Value> updated_exports;
        if (const v8::MaybeLocal<v8::Value> rval = impl::Helper::parse_json(isolate, context, p_bytes.ptr(), p_len); rval.ToLocal(&updated_exports))
        {
            p_module.exports.Reset(isolate, updated_exports);
            return true;
        }
        return false;
    }

    bool IModuleResolver::load_from_evaluator(Environment* p_env, JavaScriptModule& p_module, const String& p_asset_path, const v8::Local<v8::Function>& p_elevator)
    {
        v8::Isolate* isolate = p_env->get_isolate();
        const v8::Local<v8::Context> context = isolate->GetCurrentContext();

        // use resource path here (begins with `res://`)
        // to make path identification easier during exporting
        // (see `GodotJSExportPlugin::export_compiled_script`)
        const String& filename = p_asset_path;
        const String dirname = internal::PathUtil::dirname(filename);
        const v8::Local<v8::Object> module_obj = p_module.module.Get(isolate);

        static constexpr int kIndexExports = 0;
        static constexpr int kIndexFileName = 3;
        static constexpr int kIndexPath = 4;
        v8::Local<v8::Value> argv[] = {
            /* 0: exports  */ p_module.exports.Get(isolate),
            /* 1: require  */ p_env->_new_require_func(p_module.id),
            /* 2: module   */ module_obj,
            /* 3: filename */ impl::Helper::new_string(isolate, filename),
            /* 4: dirname  */ impl::Helper::new_string(isolate, dirname),
        };

        // init module properties (filename, path)
        module_obj->Set(context, jsb_name(p_env, filename), argv[kIndexFileName]).Check();
        module_obj->Set(context, jsb_name(p_env, path), argv[kIndexPath]).Check();

        //TODO set `require.cache`
        // ...

        if (const v8::MaybeLocal<v8::Value> result = p_elevator->Call(context, v8::Undefined(isolate), ::std::size(argv), argv);
            result.IsEmpty())
        {
            // failed, usually means error thrown
            return false;
        }

        // update `exports`, because its value may be covered during the execution process of the elevator script.
        const v8::Local<v8::Value> updated_exports = module_obj->Get(context, jsb_name(p_env, exports)).ToLocalChecked();
        jsb_unused(kIndexExports);
        // jsb_notice(updated_exports == argv[kIndexExports], "`exports` is overwritten in module: %s", filename);

        p_module.exports.Reset(isolate, updated_exports);
        return true;
    }

    size_t DefaultModuleResolver::read_all_bytes_with_shebang(const internal::ISourceReader& p_reader, Vector<uint8_t>& o_bytes)
    {
        static constexpr char header[] = "(function(exports,require,module,__filename,__dirname){";
        static constexpr char footer[] = "\n})";

        jsb_check(!p_reader.is_null());
        const size_t file_len = p_reader.get_length();
        jsb_check(file_len);
        o_bytes.resize((int) (
            file_len +
            ::std::size(header) + ::std::size(footer) - 2
            + 1 // zero_terminated anyway
        ));

        memcpy(o_bytes.ptrw(), header, ::std::size(header) - 1);
        p_reader.get_buffer(o_bytes.ptrw() + ::std::size(header) - 1, file_len);
        memcpy(o_bytes.ptrw() + file_len + ::std::size(header) - 1, footer, ::std::size(footer)); // include the ending zero
        return o_bytes.size() - 1;
    }

    //NOTE !!! we use FileAccess::exists instead of access->file_exists because access->file_exists does not consider files from packages (res://)
    bool DefaultModuleResolver::check_implicit_source_path(Environment* p_env, const String& p_module_id, String& o_path)
    {
        // try .js
        const String js_path = internal::PathUtil::extends_with(p_module_id, "." JSB_JAVASCRIPT_EXT);
        if (FileAccess::exists(js_path)
#if JSB_WITH_WEB
            || is_web_host_require_candidate(p_env, js_path)
#endif
            )
        {
            o_path = js_path;
            return true;
        }

        // try .cjs
        const String cjs_path = internal::PathUtil::extends_with(p_module_id, "." JSB_COMMONJS_EXT);
        if (FileAccess::exists(cjs_path)
#if JSB_WITH_WEB
            || is_web_host_require_candidate(p_env, cjs_path)
#endif
            )
        {
            o_path = cjs_path;
            return true;
        }

        // try .mjs
        const String mjs_path = internal::PathUtil::extends_with(p_module_id, "." JSB_MODULE_EXT);
        if (FileAccess::exists(mjs_path)
#if JSB_WITH_WEB
            || is_web_host_require_candidate(p_env, mjs_path)
#endif
            )
        {
            o_path = mjs_path;
            return true;
        }

        // try .json
        const String json_path = internal::PathUtil::extends_with(p_module_id, "." JSB_JSON_EXT);
        if (FileAccess::exists(json_path)
#if JSB_WITH_WEB
            || is_web_host_require_candidate(p_env, json_path)
#endif
            )
        {
            o_path = json_path;
            return true;
        }

        return false;
    }

    String DefaultModuleResolver::resolve_package_export_value(const Variant& p_value, const String& p_condition, const String& p_subpath, bool p_wildcard)
    {
        switch (p_value.get_type())
        {
            case Variant::STRING:
            {
                const String resolved_value = p_value;

                if (!resolved_value.begins_with("./"))
                {
                    // Not a file path. It's protocol based resolution which is unsupported (not seen in practice).
                    JSB_LOG(VeryVerbose, "Unable to resolve package.json export: %s", p_value);
                    return String();
                }

                return p_wildcard
                    ? resolved_value.replace("*", p_subpath)
                    : resolved_value + p_subpath;
            }

            case Variant::DICTIONARY:
            {
                const Dictionary condition_dict = p_value;

                if (!p_condition.is_empty() && condition_dict.has(p_condition))
                {
                    const Variant& condition_target = condition_dict[p_condition];
                    const String resolved_path = resolve_package_export_value(condition_target, p_condition, p_subpath, p_wildcard);

                    if (!resolved_path.is_empty())
                    {
                        return resolved_path;
                    }

                    if (condition_target.get_type() == Variant::NIL)
                    {
                        // Further resolution explicitly disabled i.e. don't fall back to "default"
                        return String();
                    }
                }

                const String key_default = "default";

                if (condition_dict.has(key_default))
                {
                    const String resolved_path = resolve_package_export_value(condition_dict[key_default], p_condition, p_subpath, p_wildcard);

                    if (!resolved_path.is_empty())
                    {
                        return resolved_path;
                    }
                }

                return String(); // No match
            }

            case Variant::ARRAY:
            {
                const Array alternatives = p_value;
                for (int i = 0; i < alternatives.size(); ++i)
                {
                    const Variant& alternative = alternatives[i];
                    const String resolved_path = resolve_package_export_value(alternative, p_condition, p_subpath, p_wildcard);

                    if (!resolved_path.is_empty())
                    {
                        return resolved_path;
                    }

                    if (alternative.get_type() == Variant::NIL)
                    {
                        return String(); // Further resolution explicitly disabled. Used when nested in conditions.
                    }
                }

                return String(); // No matches
            }

            case Variant::NIL:
                return String(); // Resolution explicitly disabled

            default:
                JSB_LOG(Warning, "invalid package.json exports value. Expected string, object, string[] or null. Encountered value: %s", p_value);
                return String();
        }
    }


    String DefaultModuleResolver::resolve_package_export(const Dictionary& p_exports, const String& p_condition, const String& p_module_id)
    {
        String lookup_key = p_module_id.is_empty() || p_module_id == "."
            ? "."
            : "./" + p_module_id;

        String longest_match_key;
        String longest_match_subpath;
        int longest_match_length = -1;
        bool longest_match_has_wildcard = false;

        const Array keys = p_exports.keys();
        for (int i = 0; i < keys.size(); ++i)
        {
            const String key = keys[i];
            const int wildcard_index = key.find("*");
            const String effective_key = key.substr(0, wildcard_index);

            if (lookup_key.begins_with(effective_key) && (effective_key.ends_with("/") || effective_key == lookup_key))
            {
                 const int current_match_len = effective_key.length();
                 if (current_match_len > longest_match_length)
                 {
                     longest_match_length = current_match_len;
                     longest_match_key = key;
                     longest_match_subpath = lookup_key.substr(current_match_len);
                     longest_match_has_wildcard = wildcard_index >= 0;
                 }
            }
        }

        if (longest_match_key.is_empty())
        {
            return String();
        }

        return resolve_package_export_value(p_exports[longest_match_key], p_condition, longest_match_subpath, longest_match_has_wildcard);
    }


    bool DefaultModuleResolver::check_absolute_file_path(Environment* p_env, const String& p_module_id, ModuleSourceInfo& o_source_info)
    {
        // 1: module_id (we do not check it strictly here, but usually, it should already have a valid extension)
        const bool has_known_source_extension =
            p_module_id.ends_with("." JSB_JAVASCRIPT_EXT)
            || p_module_id.ends_with("." JSB_COMMONJS_EXT)
            || p_module_id.ends_with("." JSB_MODULE_EXT)
            || p_module_id.ends_with("." JSB_JSON_EXT);

        if (has_known_source_extension &&
            (FileAccess::exists(p_module_id)
#if JSB_WITH_WEB
            || is_web_host_require_candidate(p_env, p_module_id)
#endif
            ))
        {
            o_source_info.source_filepath = p_module_id;
            o_source_info.package_filepath = String();
            return true;
        }

        // 2: implicit file path (module_id.js, module_id.cjs)
        String source_path;
        if (!has_known_source_extension && check_implicit_source_path(p_env, p_module_id, source_path))
        {
            o_source_info.source_filepath = source_path;
            o_source_info.package_filepath = String();
            JSB_LOG(Verbose, "checked file path %s", source_path);
            return true;
        }

        // 3: package.json + index fallback for directories
        if (check_package_file_path(p_env, p_module_id, String(), o_source_info))
        {
            return true;
        }

        if (check_implicit_source_path(p_env, internal::PathUtil::combine(p_module_id, "index"), source_path))
        {
            o_source_info.source_filepath = source_path;
            o_source_info.package_filepath = String();
            return true;
        }

        return false;
    }

    bool DefaultModuleResolver::check_package_file_path(Environment* p_env, const String& p_package_path, const String& p_module_id, ModuleSourceInfo& o_source_info)
    {
        if (!DirAccess::exists(p_package_path))
        {
            return false;
        }

        Dictionary package_exports;

        if (package_exports_cache.has(p_package_path))
        {
            package_exports = package_exports_cache[p_package_path];
        }
        else
        {
            const String package_json_path = internal::PathUtil::combine(p_package_path, "package.json");

            if (FileAccess::exists(package_json_path))
            {
                const Ref<FileAccess> file = FileAccess::open(package_json_path, FileAccess::READ);
                jsb_check(file.is_valid());

                const Ref json = memnew(JSON);
                Error error = json->parse(file->get_as_utf8_string());
                if (error != OK)
                {
                    JSB_LOG(Error, "failed to parse package.json (%d: %s)", json->get_error_line(), json->get_error_message());
                }

                const Dictionary& package_json = json->get_data();
                const String key_exports = "exports";

                if (package_json.has(key_exports))
                {
                    const Variant& exports = package_json[key_exports];
                    const Variant::Type exports_type = exports.get_type();

                    // Detected abbreviated exports and abbreviations to standard form

                    if (exports_type == Variant::DICTIONARY)
                    {
                        const Dictionary exports_dict = exports;
                        const String first_key = exports_dict.get_key_at_index(0);

                        if (!first_key.begins_with("."))
                        {
                            // Abbreviated conditional
                            package_exports["."] = exports_dict;
                        }
                        else
                        {
                            // Already in standard form
                            package_exports = exports_dict;
                        }
                    }
                    else if (exports_type == Variant::STRING || exports_type == Variant::ARRAY)
                    {
                        // Abbreviated value / alternatives
                        package_exports["."] = exports;
                    }
                    else if (exports_type != Variant::NIL) // Exports may be explicitly disabled with exports: null
                    {
                        JSB_LOG(Warning, "invalid package.json exports. Module imports will fail: %s", package_json_path);
                    }
                }
                else
                {
                    const String key_main = "main";

                    if (package_json.has(key_main))
                    {
                        const String dot = ".";
                        const String dot_slash = "./";
                        const String& main = package_json[key_main];

                        // Transform main to equivalent exports
                        package_exports[dot] = main.begins_with(dot) ? main : internal::PathUtil::combine(dot, main);
                        package_exports[dot_slash] = dot_slash;
                    }
                }
            }

            package_exports_cache[p_package_path] = package_exports;
        }

        if (package_exports.is_empty())
        {
            if (p_module_id.is_empty())
            {
                // Avoid infinite recursion for package root lookups: callers already attempt directory index fallback.
                return false;
            }

            return this->check_absolute_file_path(p_env, internal::PathUtil::combine(p_package_path, p_module_id), o_source_info);
        }

        const String relative_exported_path = resolve_package_export(package_exports, "require", p_module_id);

        if (relative_exported_path.is_empty())
        {
            return false;
        }

        const String exported_path = internal::PathUtil::combine(p_package_path, relative_exported_path);
        String extracted_path;
        Error error = internal::PathUtil::extract(exported_path, extracted_path);
        if (error != OK)
        {
            JSB_LOG(Error, "unrecognized exports path [%s] in %s", relative_exported_path, p_package_path);
            return false;
        }

        if (!FileAccess::exists(extracted_path)
#if JSB_WITH_WEB
            && !is_web_host_require_candidate(p_env, extracted_path)
#endif
            )
        {
            return false;
        }

        o_source_info.source_filepath = extracted_path;
        o_source_info.package_filepath = internal::PathUtil::combine(p_package_path, "package.json");
        return true;
    }

    // early and simple validation: check source file existence
    bool DefaultModuleResolver::get_source_info(Environment* p_env, const String &p_module_id, ModuleSourceInfo& r_source_info)
    {
        JSB_LOG(VeryVerbose, "resolving path %s", p_module_id);

        // directly inspect it at first if it's an absolute path
        if (internal::PathUtil::is_absolute_path(p_module_id))
        {
            if (check_absolute_file_path(p_env, p_module_id, r_source_info))
            {
                return true;
            }
            r_source_info = {};
            JSB_LOG(Verbose, "module candidate not found (absolute probe): %s", p_module_id);
            return false;
        }

        // it's a simplified implementation of the Node.js module resolution algorithm
        //   1. explicit-file
        //   2. if (is dir) dir/package.json :[main || index]
        //   3. implicit-file (path.js, path.cjs, path/index.js, path/index.cjs)
        //   X. iterate in all search paths

        // search the statically configured paths
        for (const String& search_path : search_paths_)
        {
            if (check_search_path(p_env, search_path, p_module_id, r_source_info))
            {
                return true;
            }
        }

        // search the paths from settings
        for (const String& search_path : get_dynamic_search_paths())
        {
            if (check_search_path(p_env, search_path, p_module_id, r_source_info))
            {
                return true;
            }
        }

        r_source_info = {};
        return false;
    }

    bool DefaultModuleResolver::check_search_path(Environment* p_env, const String& p_search_path, const String& p_module_id, ModuleSourceInfo& o_source_info)
    {
        if (p_module_id[0] != '.')
        {
            int package_name_slash_index = p_module_id.find_char('/');

            if (p_module_id[0] == '@' && package_name_slash_index >= 0)
            {
                package_name_slash_index = p_module_id.find_char('/', package_name_slash_index + 1);
            }

            String package_name = p_module_id.substr(0, package_name_slash_index);

            if (!package_name.is_empty())
            {
                const String absolute_package_path = internal::PathUtil::combine(p_search_path, package_name);
                const String file_path = p_module_id.substr(package_name.length() + 1);

                if (check_package_file_path(p_env, absolute_package_path, file_path, o_source_info))
                {
                    return true;
                }
            }
        }

        const String absolute_file_path = internal::PathUtil::combine(p_search_path, p_module_id);

        if (check_absolute_file_path(p_env, absolute_file_path, o_source_info))
        {
            return true;
        }

        JSB_LOG(Verbose, "module candidate not found (search_path: %s) %s", p_search_path, p_module_id);
        return false;
    }

    DefaultModuleResolver& DefaultModuleResolver::add_search_path(const String& p_path)
    {
        String normalized;
        const Error err = internal::PathUtil::extract(p_path, normalized);
        jsb_unused(err);
        jsb_checkf(err == OK, "failed to extract path when adding search path %s (%s)", p_path, jsb_ext_error_string(err));
        search_paths_.append(normalized);
        JSB_LOG(Verbose, "add search path: %s", normalized);
        return *this;
    }

    bool DefaultModuleResolver::load(Environment* p_env, const String& p_asset_path, JavaScriptModule& p_module)
    {
#if JSB_WITH_WEB
        if (!FileAccess::exists(p_asset_path) && is_web_host_require_candidate(p_env, p_asset_path))
        {
            return load_with_web_host_require(p_env, p_module, p_asset_path);
        }
#endif
        internal::FileAccessSourceReader reader(p_asset_path);
        return load(p_env, p_asset_path, reader, p_module);
    }

    bool DefaultModuleResolver::load(Environment* p_env, const String& p_asset_path, const internal::ISourceReader& p_reader, JavaScriptModule& p_module)
    {
        if (p_reader.is_null() || p_reader.get_length() == 0)
        {
            jsb_throw(p_env->get_isolate(), "failed to read module source");
            return false;
        }

#if JSB_SUPPORT_RELOAD && defined(TOOLS_ENABLED)
        p_module.time_modified = p_reader.get_time_modified();
        p_module.hash = p_reader.get_hash();
#endif

        // parse as JSON
        if (p_asset_path.ends_with("." JSB_JSON_EXT))
        {
            Vector<uint8_t> source;
            const uint64_t len = p_reader.get_length();
            source.resize((int) len + 1);
            source.write[(int) len] = 0; // ensure it's zero-terminated
            p_reader.get_buffer(source.ptrw(), len);
            return load_as_json(p_env, p_module, p_asset_path, source, len);
        }

#if JSB_DEBUG
        if (!internal::PathUtil::is_recognized_javascript_extension(p_asset_path))
        {
            JSB_LOG(Warning, "%s is suspiciously not JS", p_asset_path);
        }
#endif
        {
            v8::Isolate* isolate = p_env->get_isolate();
            v8::Isolate::Scope isolate_scope(isolate);
            v8::HandleScope handle_scope(isolate);
            v8::Local<v8::Context> context = isolate->GetCurrentContext();
            v8::Context::Scope context_scope(context);

            const String source_url = p_reader.get_source_url();
            Vector<uint8_t> source;
            const size_t len = read_all_bytes_with_shebang(p_reader, source);
            jsb_check((size_t)(int)len == len);

            // source evaluator (the module protocol)
            v8::MaybeLocal<v8::Value> func_maybe;
#if JSB_WITH_V8
            const V8CodeCacheState code_cache = load_v8_code_cache(source_url, source.ptr(), len);
            impl::Helper::CompileCacheOptions cache_options;
            cache_options.produce_cache = code_cache.write_enabled;
            if (code_cache.loaded.hit)
            {
                cache_options.data = code_cache.loaded.cache_bytes.ptr();
                cache_options.length = code_cache.loaded.cache_bytes.size();
            }

            impl::Helper::CompileCacheResult cache_result;
            func_maybe = impl::Helper::compile_function_with_cache(context, (const char*) source.ptr(), (int) len, source_url, cache_options, cache_result);
            if (func_maybe.IsEmpty() && code_cache.loaded.hit && cache_result.cache_rejected)
            {
                if (should_log_v8_code_cache())
                {
                    JSB_LOG(Warning, "V8 code cache rejected by V8; retrying without cache: %s", code_cache.loaded.path);
                }

                if (code_cache.loaded.delete_on_reject)
                {
                    remove_v8_code_cache_file(code_cache.loaded.path);
                }

                impl::Helper::CompileCacheOptions retry_cache_options;
                retry_cache_options.produce_cache = code_cache.write_enabled;
                impl::Helper::CompileCacheResult retry_cache_result;
                func_maybe = impl::Helper::compile_function_with_cache(context, (const char*) source.ptr(), (int) len, source_url, retry_cache_options, retry_cache_result);
                cache_result.produced_cache = retry_cache_result.produced_cache;
                cache_result.cache_rejected = retry_cache_result.cache_rejected;
            }

            if (!func_maybe.IsEmpty())
            {
                if (cache_result.cache_rejected && should_log_v8_code_cache())
                {
                    JSB_LOG(Verbose, "V8 code cache rejected: %s", code_cache.loaded.path);
                }
                if (cache_result.cache_rejected && code_cache.loaded.delete_on_reject)
                {
                    remove_v8_code_cache_file(code_cache.loaded.path);
                }
                if (code_cache.write_enabled && cache_result.produced_cache.is_empty() && should_log_v8_code_cache())
                {
                    JSB_LOG(Verbose, "V8 compile succeeded without producing cache bytes: %s", source_url);
                }
                store_v8_code_cache(code_cache, cache_result.produced_cache);
            }
#else
            func_maybe = impl::Helper::compile_function(context, (const char*) source.ptr(), (int) len, source_url);
#endif
            if (func_maybe.IsEmpty())
            {
                static constexpr size_t kDiagnosticPrefixMaxBytes = 192;
                const size_t diagnostic_prefix_len = std::min(len, kDiagnosticPrefixMaxBytes);
                String diagnostic_prefix;
                if (diagnostic_prefix_len > 0)
                {
                    PackedByteArray diagnostic_bytes;
                    diagnostic_bytes.resize((int) diagnostic_prefix_len);
                    memcpy(diagnostic_bytes.ptrw(), source.ptr(), diagnostic_prefix_len);
                    diagnostic_prefix = String::utf8((const char*) diagnostic_bytes.ptr(), (int) diagnostic_prefix_len);
                }
                JSB_LOG(Warning,
                    "module compile failed asset_path='%s' source_url='%s' source_len=%s prefix='%s'",
                    p_asset_path,
                    source_url,
                    String::num_uint64((uint64_t) len),
                    diagnostic_prefix);
                //NOTE an exception should have been thrown in _compile_run if MaybeLocal is empty
                return false;
            }

            v8::Local<v8::Value> func;
            if (!func_maybe.ToLocal(&func) || !func->IsFunction())
            {
                jsb_throw(p_env->get_isolate(), "bad module elevator");
                return false;
            }

            return load_from_evaluator(p_env, p_module, p_asset_path, func.As<v8::Function>());
        }
    }

}
