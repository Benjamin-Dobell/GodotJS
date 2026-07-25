import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const webHandlePath = join(moduleRoot, "impl/web/jsb_web_handle.h");
const webInteropPath = join(moduleRoot, "impl/web/jsb_web_interop.h");
const webBrokerPath = join(moduleRoot, "impl/web/jsb_web_broker.cpp");
const environmentPath = join(moduleRoot, "bridge/jsb_environment.h");
const quickjsHandlePath = join(moduleRoot, "impl/quickjs/jsb_quickjs_handle.h");
const jscHandlePath = join(moduleRoot, "impl/jsc/jsb_jsc_handle.h");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const webHandleSource = readFileSync(webHandlePath, "utf8");
const webInteropSource = readFileSync(webInteropPath, "utf8");
const webBrokerSource = readFileSync(webBrokerPath, "utf8");
const environmentSource = readFileSync(environmentPath, "utf8");
const quickjsHandleSource = readFileSync(quickjsHandlePath, "utf8");
const jscHandleSource = readFileSync(jscHandlePath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

assert.match(monolithSource, /const jsbb_WebBridgeProfileVersion = 3;/);
assert.match(monolithSource, /\bHandleResolveAndPush\b/);
assert.match(monolithSource, /"handle_resolve_and_push"/);
assert.match(
    webInteropSource,
    /JSBROWSER_API jsb::impl::StackPosition jsbi_handle_ResolveAndPushStack\(/,
);
assert.match(librarySource, /"jsbi_handle_ResolveAndPushStack": function \(engine_id, handle\)/);

const webGlobalSource = sourceBetween(
    webHandleSource,
    "template <typename T>\n    class Global\n",
    "template <typename T>\n    template <typename S>\n",
);
const webGetSource = sourceBetween(
    webGlobalSource,
    "Local<T> Get(Isolate* isolate) const",
    "template <typename S>\n        bool operator==",
);
assert.match(webGetSource, /jsb_check\(isolate_ == isolate && isolate_\);/);
assert.match(webGetSource, /if \(jsb_unlikely\(!isolate_ \|\| isolate_ != isolate\)\)/);
assert.match(webGetSource, /return \{\};/);
assert.match(webGetSource, /jsbi_handle_ResolveAndPushStack/);
assert.match(webGetSource, /if \(jsb_unlikely\(stack_pos < 0\)\)/);
assert.match(webGetSource, /jsb_check\(weak_type_ != WeakType::kStrong\);/);
assert.doesNotMatch(webGetSource, /is_alive\(\)|jsbi_handle_IsValid|jsbi_handle_PushStack/);

const tryGetObjectSource = sourceBetween(
    environmentSource,
    "jsb_force_inline bool try_get_object(T p_key, v8::Local<v8::Object>& r_unwrap) const",
    "jsb_force_inline v8::Local<v8::Object> get_object",
);
assert.match(
    tryGetObjectSource,
    /#if JSB_WITH_WEB\s+r_unwrap = ptr->ref_\.Get\(isolate_\);\s+return !r_unwrap\.IsEmpty\(\);\s+#else/,
    "Web object lookup must let the compound Global::Get resolve liveness and push once",
);
assert.match(
    tryGetObjectSource,
    /#else\s+if \(jsb_unlikely\(ptr->ref_\.IsEmpty\(\)\)\)\s+\{\s+return false;\s+\}\s+r_unwrap = ptr->ref_\.Get\(isolate_\);\s+return !r_unwrap\.IsEmpty\(\);\s+#endif/,
    "native runtimes must retain the explicit liveness guard before Global::Get",
);

for (const [backend, source] of [
    ["QuickJS", quickjsHandleSource],
    ["JavaScriptCore", jscHandleSource],
]) {
    const nativeGlobalSource = sourceBetween(
        source,
        "template <typename T>\n    class Global\n",
        "template <typename T>\n    template <typename S>\n",
    );
    const nativeGetSource = sourceBetween(
        nativeGlobalSource,
        "Local<T> Get(Isolate* isolate) const",
        "template <typename S>\n        bool operator==",
    );
    assert.match(nativeGetSource, /jsb_check\(isolate_ == isolate && isolate_ && is_alive\(\)\);/, `${backend} liveness assertion changed`);
    assert.match(nativeGetSource, /Broker::push_copy/, `${backend} native push changed`);
    assert.doesNotMatch(nativeGetSource, /ResolveAndPush/, `${backend} must not use the Web compound ABI`);
}

const callbackWeakSource = sourceBetween(
    webGlobalSource,
    "template<typename S>\n        void SetWeak(S* parameter",
    "bool IsWeak() const",
);
assert.match(callbackWeakSource, /Broker::SetWeakCallback/);
assert.doesNotMatch(callbackWeakSource, /ResolveAndPush/);
assert.match(webBrokerSource, /jsbi_handle_PushStack\(isolate->rt\(\), value\)/);
assert.doesNotMatch(webBrokerSource, /ResolveAndPush/);

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

class ControlledWeakRef {
    static instances = [];

    constructor(target) {
        this.target = target;
        ControlledWeakRef.instances.push(this);
    }

    deref() {
        return this.target;
    }

    collect() {
        this.target = undefined;
    }
}

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
    WeakRef: ControlledWeakRef,
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
globalThis.__weakHandleResolveTest = { runtime: _jsbb_ };
${librarySource}`,
    context,
    { filename: "godotjs-web-weak-handle-resolve-test.js" },
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

const engine = context.__weakHandleResolveTest.runtime.GetEngine(0);
const profiler = context.GodotJSBridgeProfiler;
assert.equal(profiler.set_enabled(0, true), true);

function addHandle(value, weak = false) {
    const weakRefIndex = ControlledWeakRef.instances.length;
    engine.stack.EnterScope();
    let handle;
    try {
        handle = bridge.jsbi_handle_New(0, engine.stack.Push(value));
    } finally {
        engine.stack.ExitScope();
    }
    const token = ControlledWeakRef.instances[weakRefIndex];
    assert.ok(token, "handle allocation must create one controllable WeakRef");
    if (weak) {
        bridge.jsbi_handle_SetWeak(0, handle);
    }
    return { handle, token };
}

function resolveHandle(handle) {
    engine.stack.EnterScope();
    try {
        const stackPosition = bridge.jsbi_handle_ResolveAndPushStack(0, handle);
        return {
            stackPosition,
            value: stackPosition < 0 ? undefined : engine.stack.GetValue(stackPosition),
        };
    } finally {
        engine.stack.ExitScope();
    }
}

function assertResolved(actual, expected, message) {
    assert.ok(actual.stackPosition >= 0, `${message}: expected a valid stack position`);
    if (Number.isNaN(expected)) {
        assert.equal(Number.isNaN(actual.value), true, `${message}: expected NaN`);
    } else {
        assert.equal(Object.is(actual.value, expected), true, message);
    }
}

function assertFusedLiveCounters(expectedResolutions = 1) {
    const snapshot = profiler.snapshot(0);
    assert.equal(snapshot.version, 3);
    assert.equal(snapshot.counters.handle_resolve_and_push, expectedResolutions);
    assert.equal(snapshot.counters.stack_push, expectedResolutions);
    assert.equal(snapshot.counters.handle_is_valid, 0);
    assert.equal(snapshot.counters.handle_get, 0);
    assert.equal(snapshot.counters.handle_collected, 0);
}

const separableBaselineValue = { state: "separable-baseline" };
const separableBaseline = addHandle(separableBaselineValue, true);
assert.equal(profiler.reset(0), true);
assert.equal(bridge.jsbi_handle_IsValid(0, separableBaseline.handle), true);
assert.equal(bridge.jsbi_handle_IsValid(0, separableBaseline.handle), true);
engine.stack.EnterScope();
try {
    const position = bridge.jsbi_handle_PushStack(0, separableBaseline.handle);
    assert.equal(engine.stack.GetValue(position), separableBaselineValue);
} finally {
    engine.stack.ExitScope();
}
let snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_is_valid, 2);
assert.equal(snapshot.counters.handle_get, 1);
assert.equal(snapshot.counters.stack_push, 1);
assert.equal(snapshot.counters.handle_resolve_and_push, 0);
bridge.jsbi_handle_Reset(0, separableBaseline.handle);

const falsyValues = [undefined, null, false, 0, -0, "", Number.NaN];
for (const value of falsyValues) {
    const strong = addHandle(value);
    assert.equal(profiler.reset(0), true);
    assertResolved(resolveHandle(strong.handle), value, `strong falsy ${String(value)}`);
    assertFusedLiveCounters();
    bridge.jsbi_handle_Reset(0, strong.handle);

    const weak = addHandle(value, true);
    assert.equal(profiler.reset(0), true);
    assertResolved(resolveHandle(weak.handle), value, `weak falsy ${String(value)}`);
    assertFusedLiveCounters();
    bridge.jsbi_handle_Reset(0, weak.handle);
}

for (const value of [{ kind: "object" }, function liveFunction() {}]) {
    const liveWeak = addHandle(value, true);
    assert.equal(profiler.reset(0), true);
    assertResolved(resolveHandle(liveWeak.handle), value, "live traceable weak value");
    assertFusedLiveCounters();
    bridge.jsbi_handle_Reset(0, liveWeak.handle);
}

const repeatedValue = { repeated: true };
const repeated = addHandle(repeatedValue, true);
assert.equal(profiler.reset(0), true);
assertResolved(resolveHandle(repeated.handle), repeatedValue, "first repeated weak resolution");
assertResolved(resolveHandle(repeated.handle), repeatedValue, "second repeated weak resolution");
assertFusedLiveCounters(2);
bridge.jsbi_handle_Reset(0, repeated.handle);

let reentrantHandle;
let reentrantTrapCalls = 0;
const reentrantValue = new Proxy({ state: "reentrant" }, {
    getPrototypeOf(target) {
        ++reentrantTrapCalls;
        engine.stack.Push("nested-stack-value");
        bridge.jsbi_handle_Reset(0, reentrantHandle);
        return Reflect.getPrototypeOf(target);
    },
});
const reentrant = addHandle(reentrantValue);
reentrantHandle = reentrant.handle;
const countBeforeReentrantResolution = engine.handles.count;
assert.equal(profiler.reset(0), true);
engine.stack.EnterScope();
try {
    const initialPosition = engine.stack.pos;
    const resolvedPosition = bridge.jsbi_handle_ResolveAndPushStack(0, reentrant.handle);
    assert.equal(resolvedPosition, initialPosition + 1, "re-entrant stack activity must precede the outer push");
    assert.equal(engine.stack.GetValue(initialPosition), "nested-stack-value");
    assert.equal(engine.stack.GetValue(resolvedPosition), reentrantValue);
} finally {
    engine.stack.ExitScope();
}
assert.equal(reentrantTrapCalls, 1);
assert.equal(engine.handles.count, countBeforeReentrantResolution - 1, "re-entrant reset must remain effective");
snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_resolve_and_push, 1);
assert.equal(snapshot.counters.handle_reset, 1);
assert.equal(snapshot.counters.stack_push, 2);
assert.equal(snapshot.counters.handle_get, 0);
assert.equal(snapshot.counters.handle_is_valid, 0);

const unwrapError = new Error("unwrap trap");
const throwingValue = new Proxy({}, {
    getPrototypeOf() {
        throw unwrapError;
    },
});
const throwing = addHandle(throwingValue);
assert.equal(profiler.reset(0), true);
engine.stack.EnterScope();
try {
    const initialPosition = engine.stack.pos;
    assert.throws(
        () => bridge.jsbi_handle_ResolveAndPushStack(0, throwing.handle),
        (error) => error === unwrapError,
    );
    assert.equal(engine.stack.pos, initialPosition, "an unwrap exception must not push a partial result");
} finally {
    engine.stack.ExitScope();
}
snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_resolve_and_push, 1);
assert.equal(snapshot.counters.stack_push, 0);
assert.equal(snapshot.counters.handle_get, 0);
assert.equal(snapshot.counters.handle_is_valid, 0);
bridge.jsbi_handle_Reset(0, throwing.handle);

const dead = addHandle({ state: "dead" }, true);
const countBeforeDeadResolution = engine.handles.count;
dead.token.collect();
assert.equal(profiler.reset(0), true);
assert.equal(resolveHandle(dead.handle).stackPosition, -1);
assert.equal(engine.handles.count, countBeforeDeadResolution - 1);
snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_resolve_and_push, 1);
assert.equal(snapshot.counters.handle_collected, 1);
assert.equal(snapshot.counters.stack_push, 0);
assert.equal(snapshot.counters.handle_is_valid, 0);
assert.equal(snapshot.counters.handle_get, 0);

assert.equal(profiler.reset(0), true);
assert.equal(resolveHandle(dead.handle).stackPosition, -1, "a collected entry must remain missing");
assert.equal(engine.handles.count, countBeforeDeadResolution - 1);
snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_resolve_and_push, 1);
assert.equal(snapshot.counters.handle_collected, 0);
assert.equal(snapshot.counters.stack_push, 0);

const deleted = addHandle({ state: "deleted" });
const deletedHandle = deleted.handle;
bridge.jsbi_handle_Reset(0, deletedHandle);
assert.equal(profiler.reset(0), true);
assert.equal(resolveHandle(deletedHandle).stackPosition, -1);
snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_resolve_and_push, 1);
assert.equal(snapshot.counters.handle_collected, 0);
assert.equal(snapshot.counters.stack_push, 0);

const monotonic = addHandle({ state: "monotonic" });
assert.ok(monotonic.handle > deletedHandle, "compound cleanup must not recycle handle IDs");
bridge.jsbi_handle_Reset(0, monotonic.handle);

const legacy = addHandle({ state: "legacy-callback-path" });
assert.equal(profiler.reset(0), true);
engine.stack.EnterScope();
try {
    const position = bridge.jsbi_handle_PushStack(0, legacy.handle);
    assert.deepEqual(engine.stack.GetValue(position), { state: "legacy-callback-path" });
} finally {
    engine.stack.ExitScope();
}
snapshot = profiler.snapshot(0);
assert.equal(snapshot.counters.handle_resolve_and_push, 0);
assert.equal(snapshot.counters.handle_get, 1);
assert.equal(snapshot.counters.stack_push, 1);
bridge.jsbi_handle_Reset(0, legacy.handle);

bridge.jsbi_FreeEngine(0);

console.log("GodotJS Web fused weak-handle resolution test: PASS");
