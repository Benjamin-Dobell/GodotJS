import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");
const profileHeaderPath = join(moduleRoot, "impl/shared/jsb_web_bridge_profile.h");

const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");
const profileHeaderSource = readFileSync(profileHeaderPath, "utf8");

function enumMembers(source, enumName) {
    const match = source.match(new RegExp(`enum(?:\\s+class)?\\s+${enumName}[^\\{]*\\{([\\s\\S]*?)\\}`));
    assert.ok(match, `missing enum ${enumName}`);
    return match[1]
        .split(",")
        .map((entry) => entry.replace(/\/\/.*$/gm, "").trim())
        .filter(Boolean)
        .map((entry) => entry.split("=")[0].trim());
}

const cppOperations = enumMembers(profileHeaderSource, "WebBridgeOperation");
const tsOperations = enumMembers(monolithSource, "jsbb_WebBridgeOperation");
assert.deepEqual(tsOperations, cppOperations, "C++ and TypeScript operation IDs must remain ordered identically");

const cppDepth = Number(profileHeaderSource.match(/WEB_BRIDGE_PROFILE_MAX_OPERATION_DEPTH\s*=\s*(\d+)/)?.[1]);
const tsDepth = Number(monolithSource.match(/jsbb_WebBridgeProfileMaxOperationDepth\s*=\s*(\d+)/)?.[1]);
assert.equal(tsDepth, cppDepth, "C++ and TypeScript operation-depth limits must remain identical");
assert.equal(tsDepth, 16);

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
const mirroredEnabled = [];
let nextAllocation = 1024;
let profileFloat64ArrayAllocations = 0;
let profileUint8ArrayAllocations = 0;
class TrackingFloat64Array extends Float64Array {
    constructor(...args) {
        super(...args);
        ++profileFloat64ArrayAllocations;
    }
}
class TrackingUint8Array extends Uint8Array {
    constructor(...args) {
        super(...args);
        ++profileUint8ArrayAllocations;
    }
}
const context = vm.createContext({
    ArrayBuffer,
    BigInt64Array,
    ENVIRONMENT_IS_PTHREAD: false,
    FinalizationRegistry,
    HEAP8: new Int8Array(memory),
    HEAP32: new Int32Array(memory),
    HEAPU8: new Uint8Array(memory),
    LibraryManager: { library: {} },
    Map,
    Module: undefined,
    PThread: undefined,
    Promise,
    Set,
    Float64Array: TrackingFloat64Array,
    Uint8Array: TrackingUint8Array,
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
    `${transpiledMonolith.outputText}\n${librarySource}`,
    context,
    { filename: "godotjs-web-bridge-profile-test.js" },
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
    (opaque, enabled) => mirroredEnabled.push([opaque, enabled]),
    false,
    false,
);
assert.equal(bridge.jsbi_NewEngine(1234), 0);

const profiler = context.GodotJSBridgeProfiler;
assert.ok(Object.isFrozen(profiler));

const disabled = profiler.snapshot(0);
assert.equal(disabled.enabled, false);
assert.equal("counters" in disabled, false);
assert.equal("by_operation" in disabled, false);
assert.equal("gauges" in disabled, false);
assert.equal(bridge.jsbi_WebBridgeProfileOperationEnter(0, 3), 0);
assert.deepEqual(mirroredEnabled, []);
assert.equal(profileFloat64ArrayAllocations, 0, "disabled engine must not allocate profile counter arrays");
assert.equal(profileUint8ArrayAllocations, 0, "disabled engine must not allocate a profile operation stack");

assert.equal(profiler.set_enabled(0, true), true);
assert.deepEqual(mirroredEnabled, [[1234, true]]);
assert.equal(profileFloat64ArrayAllocations, 2, "enabling must allocate exactly two fixed counter arrays");
assert.equal(profileUint8ArrayAllocations, 1, "enabling must allocate exactly one fixed operation stack");

const operationId = Object.fromEntries(
    cppOperations.map((name, index) => [name, index]),
);
const javascriptGeneration = bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.JavaScriptCall);
const functionCacheGeneration = bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.FunctionCache);
assert.notEqual(javascriptGeneration, 0);
assert.equal(functionCacheGeneration, javascriptGeneration);
assert.equal(bridge.jsbi_IsUndefined(0, 0), true);
const undefinedHandle = bridge.jsbi_handle_New(0, 0);
assert.equal(bridge.jsbi_handle_eq(0, undefinedHandle, undefinedHandle), true);
bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.FunctionCache, functionCacheGeneration);
assert.equal(bridge.jsbi_IsNull(0, 1), true);
bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.JavaScriptCall, javascriptGeneration);

const attributed = profiler.snapshot(0);
assert.equal(attributed.enabled, true);
assert.equal(attributed.counters.operation_enter, 2);
assert.equal(attributed.counters.operation_exit, 2);
assert.equal(attributed.counters.type_is_undefined, 1);
assert.equal(attributed.counters.type_is_null, 1);
assert.equal(attributed.counters.handle_equals, 1);
assert.equal(attributed.by_operation.function_cache.type_is_undefined, 1);
assert.equal(attributed.by_operation.function_cache.handle_equals, 1);
assert.equal(attributed.by_operation.javascript_call.type_is_null, 1);
assert.equal(attributed.gauges.operation_depth, 0);
assert.equal(attributed.gauges.peak_operation_depth, 2);
assert.equal(attributed.gauges.handle_count, 1);
assert.equal(attributed.operation_names.length, operationId.Num);
assert.equal(attributed.counter_names.length, enumMembers(monolithSource, "jsbb_WebBridgeCounter").indexOf("Num"));

assert.equal(profiler.reset(0), true);
assert.equal(profileFloat64ArrayAllocations, 2, "reset must reuse fixed counter arrays");
assert.equal(profileUint8ArrayAllocations, 1, "reset must reuse the fixed operation stack");
const overflowGenerations = [];
for (let depth = 0; depth <= tsDepth; ++depth) {
    overflowGenerations.push(bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.JavaScriptCall));
}
assert.equal(bridge.jsbi_IsUndefined(0, 0), true);
for (let depth = tsDepth; depth >= 0; --depth) {
    bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.JavaScriptCall, overflowGenerations[depth]);
}
const overflowed = profiler.snapshot(0);
assert.equal(overflowed.counters.operation_overflow, 1);
assert.equal(overflowed.by_operation.overflow.type_is_undefined, 1);
assert.equal(overflowed.gauges.operation_depth, 0);
assert.equal(overflowed.gauges.peak_operation_depth, tsDepth + 1);

assert.equal(profiler.reset(0), true);
assert.equal(bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.None), 0);
const mismatchGeneration = bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.JavaScriptCall);
bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.FunctionCache, mismatchGeneration);
bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.JavaScriptCall, mismatchGeneration);
const invalidAndMismatched = profiler.snapshot(0);
assert.equal(invalidAndMismatched.counters.operation_invalid_tag, 1);
assert.equal(invalidAndMismatched.counters.operation_mismatch, 1);
assert.equal(invalidAndMismatched.counters.operation_underflow, 1);

assert.equal(profiler.reset(0), true);
const staleGeneration = bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.JavaScriptCall);
assert.equal(profiler.reset(0), true);
bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.JavaScriptCall, staleGeneration);
assert.equal(profiler.snapshot(0).counters.operation_stale_exit, 1);

const snapshotCopy = profiler.snapshot(0);
assert.equal(bridge.jsbi_IsUndefined(0, 0), true);
assert.equal(snapshotCopy.counters.type_is_undefined, 0, "snapshots must not expose live counter storage");
assert.equal(profiler.snapshot(0).counters.type_is_undefined, 1);

const generationBeforeDisable = bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.JavaScriptCall);
assert.equal(profiler.set_enabled(0, false), true);
assert.equal(profiler.set_enabled(0, true), true);
assert.equal(profileFloat64ArrayAllocations, 4);
assert.equal(profileUint8ArrayAllocations, 2);
bridge.jsbi_WebBridgeProfileOperationExit(0, operationId.JavaScriptCall, generationBeforeDisable);
assert.equal(profiler.snapshot(0).counters.operation_stale_exit, 1);

assert.equal(profiler.set_enabled(0, false), true);
const disabledAgain = profiler.snapshot(0);
assert.equal(disabledAgain.enabled, false);
assert.equal("counters" in disabledAgain, false);
assert.equal(bridge.jsbi_WebBridgeProfileOperationEnter(0, operationId.JavaScriptCall), 0);
assert.equal(profiler.reset(0), false);

assert.equal(profiler.set_enabled(0, true), true);
assert.equal(profileFloat64ArrayAllocations, 6);
assert.equal(profileUint8ArrayAllocations, 3);
bridge.jsbi_FreeEngine(0);
assert.deepEqual(mirroredEnabled, [
    [1234, true],
    [1234, false],
    [1234, true],
    [1234, false],
    [1234, true],
    [1234, false],
]);

console.log("GodotJS Web bridge profile test: PASS");
