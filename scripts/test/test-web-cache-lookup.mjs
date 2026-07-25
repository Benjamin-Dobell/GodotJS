import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const refPath = join(moduleRoot, "bridge/jsb_ref.h");
const stringCachePath = join(moduleRoot, "bridge/jsb_string_name_cache.h");
const environmentPath = join(moduleRoot, "bridge/jsb_environment.cpp");
const variantUtilPath = join(moduleRoot, "internal/jsb_variant_util.h");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const refSource = readFileSync(refPath, "utf8");
const stringCacheSource = readFileSync(stringCachePath, "utf8");
const environmentSource = readFileSync(environmentPath, "utf8");
const variantUtilSource = readFileSync(variantUtilPath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

const localRefSource = sourceBetween(
    refSource,
    "#if JSB_WITH_WEB\n    /**\n     * A non-owning view used only for synchronous heterogeneous map lookup.",
    "template<typename T = v8::Object>\n    struct TWeakRef",
);
assert.match(localRefSource, /#if JSB_WITH_WEB/);
assert.match(localRefSource, /template<typename T>\s+struct TLocalRef/);
assert.match(localRefSource, /v8::Local<T> object_;/);
assert.match(localRefSource, /hash_ = p_object->GetIdentityHash\(\);/);
assert.doesNotMatch(
    localRefSource,
    /v8::Global|Reset\(|SetWeak\(/,
    "a lookup view must never own or weaken a browser handle",
);
assert.match(
    variantUtilSource,
    /std::unordered_map<K, V, typename Hasher<K>::hasher, typename Hasher<K>::equaler>/,
    "TypeGen must select internal::Hasher<K>, not functors nested in the key",
);

const weakRefSource = sourceBetween(
    refSource,
    "template<typename T = v8::Object>\n    struct TWeakRef",
    "template<typename T = v8::Object>\n    struct TStrongRef",
);
const strongRefSource = sourceBetween(
    refSource,
    "template<typename T = v8::Object>\n    struct TStrongRef",
    "\n    };\n}\n",
);
for (const [name, source] of [["weak", weakRefSource], ["strong", strongRefSource]]) {
    assert.doesNotMatch(
        source,
        /using is_transparent = void;|TLocalRef<T>/,
        `${name} key must not carry dead transparent functors ignored by TypeGen`,
    );
}

const selectedHasherSource = sourceBetween(
    refSource,
    "#if JSB_WITH_WEB\nnamespace jsb::internal\n{",
    "#endif\n\n#endif",
);
const weakHasherSource = sourceBetween(
    selectedHasherSource,
    "template<typename T>\n    struct Hasher<TWeakRef<T>>",
    "template<typename T>\n    struct Hasher<TStrongRef<T>>",
);
const strongHasherSource = selectedHasherSource.slice(
    selectedHasherSource.indexOf("template<typename T>\n    struct Hasher<TStrongRef<T>>"),
);
for (const [name, source] of [["weak", weakHasherSource], ["strong", strongHasherSource]]) {
    assert.match(source, /using is_transparent = void;/, `${name} selected hasher must enable heterogeneous find`);
    assert.match(source, /operator\(\)\(const TLocalRef<T>& lhs, const T(?:Weak|Strong)Ref<T>& rhs\)/);
    assert.match(source, /operator\(\)\(const T(?:Weak|Strong)Ref<T>& lhs, const TLocalRef<T>& rhs\)/);
    assert.match(source, /lhs\.object_ == rhs\.object_|rhs\.object_ == lhs\.object_/);
}

const webStringLookups = stringCacheSource.match(
    /#if JSB_WITH_WEB\s+if \(const auto& it = value_index_\.find\(TLocalRef\(p_value\)\); it != value_index_\.end\(\)\)\s+#else\s+if \(const auto& it = value_index_\.find\(TStrongRef\(isolate, p_value\)\); it != value_index_\.end\(\)\)\s+#endif/g,
) ?? [];
assert.equal(webStringLookups.length, 2, "both Web string lookup entry points must use local views");
assert.match(
    stringCacheSource,
    /slot\.ref_ = TStrongRef\(isolate, p_value\);[\s\S]*value_index_\.insert\(std::pair\(TStrongRef\(isolate, p_value\), id\)\);/,
    "string misses must retain durable slot and map owners",
);
assert.match(
    stringCacheSource,
    /value_index_\.erase\(slot\.ref_\);/,
    "string eviction/replacement must retain owning-key cleanup",
);

const getCachedFunctionSource = sourceBetween(
    environmentSource,
    "ObjectCacheID Environment::get_cached_function",
    "Vector<StringName> Environment::scan_external_changes",
);
assert.match(
    getCachedFunctionSource,
    /#if JSB_WITH_WEB\s+const auto& it = function_refs_\.find\(TLocalRef\(p_func\)\);\s+#else\s+const auto& it = function_refs_\.find\(TWeakRef\(isolate, p_func\)\);\s+#endif/,
    "only Web function lookup may use the non-owning local view",
);
assert.match(
    getCachedFunctionSource,
    /function_bank_\.add\(TStrongRef\(isolate, p_func\)\)/,
    "a function miss must retain its durable strong owner",
);
assert.match(
    getCachedFunctionSource,
    /function_refs_\.insert\(std::pair\(TWeakRef\(isolate, p_func\), new_id\)\);/,
    "a function miss must retain its durable weak backlink",
);

const releaseFunctionSource = sourceBetween(
    environmentSource,
    "bool Environment::release_function",
    "Variant Environment::_call",
);
assert.match(
    releaseFunctionSource,
    /function_refs_\.erase\(TWeakRef\(isolate, strong_ref\.object_\)\)/,
    "function release must retain weak-key cleanup",
);
assert.match(releaseFunctionSource, /function_bank_\.remove_at_checked\(p_func_id\)/);

const transpiledMonolith = ts.transpileModule(monolithSource, {
    compilerOptions: {
        module: ts.ModuleKind.None,
        target: ts.ScriptTarget.ES2021,
        ignoreDeprecations: "6.0",
    },
    fileName: monolithPath,
    reportDiagnostics: true,
});
assert.deepEqual(
    transpiledMonolith.diagnostics?.map((diagnostic) => diagnostic.messageText) ?? [],
    [],
    "monolith transpilation must not report diagnostics",
);

const memory = new ArrayBuffer(16 * 1024);
let nextAllocation = 1024;
const context = vm.createContext({
    ArrayBuffer,
    BigInt64Array,
    ENVIRONMENT_IS_PTHREAD: false,
    FinalizationRegistry,
    Float64Array,
    HEAP8: new Int8Array(memory),
    HEAP32: new Int32Array(memory),
    HEAPU8: new Uint8Array(memory),
    LibraryManager: { library: {} },
    Map,
    Module: undefined,
    PThread: undefined,
    Promise,
    Set,
    Uint8Array,
    WeakMap,
    WeakRef,
    WebAssembly,
    console,
    globalThis: undefined,
    mergeInto: (target, source) => Object.assign(target, source),
    updateMemoryViews: () => {},
    wasmMemory: { buffer: memory },
    UTF8ToString: () => "",
    lengthBytesUTF8: (value) => Buffer.byteLength(value, "utf8"),
    stringToUTF8: () => {},
    _free: () => {},
    _malloc: (size) => {
        const allocation = nextAllocation;
        nextAllocation += size;
        return allocation;
    },
    GodotRuntime: {
        get_func: (callback) => callback,
    },
});
context.globalThis = context;

vm.runInContext(
    `${transpiledMonolith.outputText}
globalThis.__cacheLookupTest = { runtime: _jsbb_ };
${librarySource}`,
    context,
    { filename: "godotjs-web-cache-lookup-test.js" },
);

const bridge = context.LibraryManager.library;
bridge.jsbi_init(
    () => {},
    () => {},
    () => {},
    () => {},
    () => 1,
    () => {},
    () => {},
    () => 0,
    () => {},
    false,
    false,
);
assert.equal(bridge.jsbi_NewEngine(1234), 0);

const profiler = context.GodotJSBridgeProfiler;
assert.equal(profiler.set_enabled(0, true), true);
const engine = context.__cacheLookupTest.runtime.GetEngine(0);
const baselineHandleCount = engine.handles.count;

function withLocal(value, callback) {
    engine.stack.EnterScope();
    try {
        return callback(engine.stack.Push(value));
    } finally {
        engine.stack.ExitScope();
    }
}

class HeterogeneousHandleCache {
    constructor(weak) {
        this.weak = weak;
        this.buckets = new Map();
        this.entries = [];
    }

    insert(value) {
        return withLocal(value, (localPosition) => {
            const hash = bridge.jsbi_hash(0, localPosition);
            const handle = bridge.jsbi_handle_New(0, localPosition);
            if (this.weak) {
                bridge.jsbi_handle_SetWeak(0, handle);
            }
            const entry = { handle, hash };
            const bucket = this.buckets.get(hash) ?? [];
            bucket.push(entry);
            this.buckets.set(hash, bucket);
            this.entries.push(entry);
            return entry;
        });
    }

    find(value) {
        return withLocal(value, (localPosition) => {
            const hash = bridge.jsbi_hash(0, localPosition);
            for (const entry of this.buckets.get(hash) ?? []) {
                const storedPosition = bridge.jsbi_handle_PushStack(0, entry.handle);
                if (bridge.jsbi_stack_eq(0, localPosition, storedPosition)) {
                    return entry;
                }
            }
            return undefined;
        });
    }

    clear() {
        for (const entry of this.entries) {
            bridge.jsbi_handle_Reset(0, entry.handle);
        }
        this.entries = [];
        this.buckets.clear();
    }
}

const stringCache = new HeterogeneousHandleCache(false);
const functionCache = new HeterogeneousHandleCache(true);
const cachedFunction = function cachedFunction() {};
const otherFunction = function otherFunction() {};
const collisionA = function collisionA() {};
const collisionB = function collisionB() {};

const stringEntry = stringCache.insert("same-content");
const functionEntry = functionCache.insert(cachedFunction);
engine._identity_hashes.set(collisionA, 7171);
engine._identity_hashes.set(collisionB, 7171);
const collisionEntry = functionCache.insert(collisionA);
assert.equal(engine.handles.count, baselineHandleCount + 3);

assert.equal(profiler.reset(0), true);
assert.equal(stringCache.find(["same", "content"].join("-")), stringEntry);
assert.equal(functionCache.find(cachedFunction), functionEntry);
assert.equal(functionCache.find(otherFunction), undefined, "distinct functions must miss");
assert.equal(functionCache.find(collisionA), collisionEntry);
assert.equal(functionCache.find(collisionB), undefined, "strict identity must reject a same-hash function");

const hitSnapshot = profiler.snapshot(0);
assert.equal(hitSnapshot.counters.handle_new, 0, "lookup hits/misses must not allocate temporary Globals");
assert.equal(hitSnapshot.counters.handle_reset, 0, "lookup views must have no temporary handle to reset");
assert.equal(hitSnapshot.counters.handle_equals, 0, "lookup views must avoid Global-to-Global equality");
assert.ok(hitSnapshot.counters.identity_hash >= 5, "each lookup still needs a stable bucket hash");
assert.ok(hitSnapshot.counters.handle_get >= 4, "candidate equality must resolve stored handles");
assert.ok(hitSnapshot.counters.stack_push >= 9, "locals and stored candidates must stay stack-scoped");

assert.equal(profiler.reset(0), true);
const insertedAfterMiss = functionCache.insert(otherFunction);
assert.equal(functionCache.find(otherFunction), insertedAfterMiss);
const missSnapshot = profiler.snapshot(0);
assert.equal(missSnapshot.counters.handle_new, 1, "a durable weak key must still be allocated on insertion");
assert.equal(missSnapshot.counters.handle_set_weak, 1, "a durable function key must remain weak");

assert.equal(profiler.reset(0), true);
stringCache.clear();
functionCache.clear();
assert.equal(engine.handles.count, baselineHandleCount, "cache cleanup must release every durable key");
assert.equal(profiler.snapshot(0).counters.handle_reset, 4);

bridge.jsbi_FreeEngine(0);

console.log("GodotJS Web heterogeneous cache lookup test: PASS");
