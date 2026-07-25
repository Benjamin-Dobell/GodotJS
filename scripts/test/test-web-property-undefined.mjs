import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const typeConvertPath = join(moduleRoot, "bridge/jsb_type_convert.cpp");
const interopPath = join(moduleRoot, "impl/web/jsb_web_interop.h");
const objectHeaderPath = join(moduleRoot, "impl/web/jsb_web_object.h");
const objectSourcePath = join(moduleRoot, "impl/web/jsb_web_object.cpp");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const typeConvertSource = readFileSync(typeConvertPath, "utf8");
const interopSource = readFileSync(interopPath, "utf8");
const objectHeaderSource = readFileSync(objectHeaderPath, "utf8");
const objectSource = readFileSync(objectSourcePath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

assert.match(
    interopSource,
    /typedef int StackPosition;/,
    "the tagged scalar relies on the existing signed-int Web stack-position ABI",
);
assert.match(monolithSource, /type StackPosition = number;/);
const stackSource = sourceBetween(monolithSource, "class jsbb_Stack<T>", "class jsbb_Handles");
assert.match(
    stackSource,
    /if \(pos < 0 \|\| pos >= this\.values\.length\)/,
    "negative values must remain outside the valid stack-position domain",
);
assert.match(
    stackSource,
    /const position = this\.values\.push\(val\) - 1;/,
    "valid stack positions must remain zero-based and nonnegative",
);

assert.match(
    interopSource,
    /JSBROWSER_API jsb::impl::StackPosition jsbi_GetPropertyAndCheckUndefined\(jsb::impl::JSRuntime engine_id, jsb::impl::StackPosition obj_sp, jsb::impl::StackPosition key_sp\);/,
    "the compound ABI must return both results in one scalar without a Wasm-memory out-parameter",
);
assert.doesNotMatch(
    interopSource,
    /jsbi_GetPropertyAndCheckUndefined\([^;]*\*/,
    "the compound ABI must not carry any pointer parameter",
);
assert.equal(
    librarySource.match(/"jsbi_GetPropertyAndCheckUndefined": function/g)?.length ?? 0,
    1,
    "the Emscripten library must expose exactly one compound property/undefined forwarder",
);
assert.match(
    librarySource,
    /"jsbi_GetPropertyAndCheckUndefined": function \(engine_id, obj_sp, key_sp\) \{ return _jsbb_\.GetEngine\(engine_id\)\.GetPropertyAndCheckUndefined\(obj_sp, key_sp\); \}/,
    "the Emscripten forwarder must not pass a heap pointer",
);
assert.match(
    objectHeaderSource,
    /MaybeLocal<Value> GetAndCheckUndefined\(Local<Context> context, Local<Value> key, bool& r_is_undefined\) const;/,
);

const objectMethodSource = sourceBetween(
    objectSource,
    "MaybeLocal<Value> Object::GetAndCheckUndefined",
    "Maybe<bool> Object::HasOwnProperty",
);
assert.equal(
    objectMethodSource.match(/jsbi_GetPropertyAndCheckUndefined/g)?.length ?? 0,
    1,
    "the Web facade must cross the compound ABI exactly once",
);
assert.doesNotMatch(
    objectMethodSource,
    /int32_t|is_undefined_ptr|&is_undefined/,
    "the C++ facade must not allocate or pass a Wasm-memory out-parameter",
);
assert.match(
    objectMethodSource,
    /const jsb::impl::StackPosition encoded_rval = jsbi_GetPropertyAndCheckUndefined\(isolate_->rt\(\), stack_pos_, key->stack_pos_\);/,
);
assert.match(
    objectMethodSource,
    /if \(encoded_rval == jsb::impl::StackBase::Error\)\s*\{\s*return MaybeLocal<Value>\(\);\s*\}/,
    "an Error stack position must remain an empty MaybeLocal",
);
assert.match(
    objectMethodSource,
    /r_is_undefined = encoded_rval < 0;\s*const jsb::impl::StackPosition rval = r_is_undefined \? ~encoded_rval : encoded_rval;/,
    "the facade must decode the full-range signed complement before constructing the Local",
);
assert.match(objectMethodSource, /return MaybeLocal<Value>\(Data\(isolate_, rval\)\);/);

const monolithMethodSource = sourceBetween(
    monolithSource,
    "GetPropertyAndCheckUndefined(",
    "GetPropertyUint32(",
);
assert.equal(
    monolithMethodSource.match(/this\.GetProperty\(obj_sp, key_sp\)/g)?.length ?? 0,
    1,
    "the compound method must reuse the authoritative property getter exactly once",
);
assert.match(
    monolithMethodSource,
    /if \(rval === jsbb_StackPos\.Error\)\s*\{\s*return rval;\s*\}/,
    "property errors must return before the undefined operation",
);
assert.match(monolithMethodSource, /jsbb_WebBridgeCounter\.TypeIsUndefined/);
assert.equal(
    monolithMethodSource.match(/this\._stack\.GetValue\(rval\)/g)?.length ?? 0,
    1,
    "the compound method must preserve the one live post-property stack read",
);
assert.match(
    monolithMethodSource,
    /return this\._stack\.GetValue\(rval\) === undefined \? ~rval : rval;/,
    "live undefined results must complement the original position into the scalar return",
);
assert.doesNotMatch(
    monolithMethodSource,
    /obj\[|Map|\.get\(|_stack\.Push|_throw\(|_jsbb_|HEAP|\\.i32|Pointer|o_is_undefined/,
    "the compound wrapper must not duplicate property work or access an Emscripten heap view",
);

assert.equal(
    typeConvertSource.match(/GetAndCheckUndefined\(/g)?.length ?? 0,
    3,
    "all three measured ProxyTarget conversion sites must use the compound Web operation",
);
assert.equal(
    typeConvertSource.match(/!target->IsUndefined\(\)/g)?.length ?? 0,
    3,
    "native V8/JSC/QuickJS conversion paths must retain their existing undefined tests",
);
assert.equal(
    typeConvertSource.match(
        /#if JSB_WITH_WEB[\s\S]{0,320}GetAndCheckUndefined\([\s\S]{0,320}#else[\s\S]{0,320}->Get\(/g,
    )?.length ?? 0,
    3,
    "each compound lookup must be isolated to JSB_WITH_WEB with the native getter retained",
);

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

const memory = new ArrayBuffer(32 * 1024);
let nextAllocation = 4096;
const bridgeErrorLogs = [];
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
    Proxy,
    Reflect,
    Set,
    Symbol,
    Uint8Array,
    WeakMap,
    WeakRef,
    WebAssembly,
    console: {
        ...console,
        error: (...args) => bridgeErrorLogs.push(args),
    },
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
globalThis.__propertyUndefinedTest = {
    push(value) {
        return _jsbb_.GetEngine(0).stack.Push(value);
    },
    get(stackPosition) {
        return _jsbb_.GetEngine(0).stack.GetValue(stackPosition);
    },
    set(stackPosition, value) {
        _jsbb_.GetEngine(0).stack.SetValue(stackPosition, value);
    },
    encodeSyntheticUndefined(stackPosition) {
        const engine = _jsbb_.GetEngine(0);
        const originalGetProperty = engine.GetProperty;
        const originalGetValue = engine.stack.GetValue;
        engine.GetProperty = () => stackPosition;
        engine.stack.GetValue = (position) => position === stackPosition
            ? undefined
            : originalGetValue.call(engine.stack, position);
        try {
            return engine.GetPropertyAndCheckUndefined(0, 0);
        } finally {
            engine.GetProperty = originalGetProperty;
            engine.stack.GetValue = originalGetValue;
        }
    },
    mutateNextPropertyResult(value) {
        const engine = _jsbb_.GetEngine(0);
        const originalGetProperty = engine.GetProperty;
        engine.GetProperty = function (...args) {
            const resultPosition = originalGetProperty.apply(this, args);
            this.stack.SetValue(resultPosition, value);
            engine.GetProperty = originalGetProperty;
            return resultPosition;
        };
    },
    stackPositions: { ...jsbb_StackPos },
};
${librarySource}`,
    context,
    { filename: "godotjs-web-property-undefined-test.js" },
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
const fixture = context.__propertyUndefinedTest;
const stackPositions = fixture.stackPositions;
const proxyTarget = Symbol("ProxyTarget");
const keyPosition = fixture.push(proxyTarget);

function compoundGet(object) {
    const objectPosition = fixture.push(object);
    assert.equal(profiler.reset(0), true);
    const encodedResult = bridge.jsbi_GetPropertyAndCheckUndefined(
        0,
        objectPosition,
        keyPosition,
    );
    const isUndefined = encodedResult < 0;
    const resultPosition = isUndefined ? ~encodedResult : encodedResult;
    return {
        encodedResult,
        resultPosition,
        isUndefined,
        snapshot: profiler.snapshot(0),
    };
}

let result = compoundGet({});
assert.equal(result.encodedResult, ~stackPositions.Undefined);
assert.equal(result.resultPosition, stackPositions.Undefined);
assert.equal(result.isUndefined, true);
assert.equal(result.snapshot.counters.property_get, 1);
assert.equal(result.snapshot.counters.type_is_undefined, 1);
assert.equal(result.snapshot.counters.stack_get, 3);
assert.equal(result.snapshot.counters.stack_push, 0);

result = compoundGet({ [proxyTarget]: null });
assert.equal(result.encodedResult, stackPositions.Null);
assert.equal(result.resultPosition, stackPositions.Null);
assert.equal(result.isUndefined, false);
assert.equal(result.snapshot.counters.property_get, 1);
assert.equal(result.snapshot.counters.type_is_undefined, 1);
assert.equal(result.snapshot.counters.stack_get, 3);
assert.equal(result.snapshot.counters.stack_push, 0);

const targetValue = { target: true };
result = compoundGet({ [proxyTarget]: targetValue });
assert.ok(result.resultPosition > stackPositions.Error);
assert.equal(result.encodedResult, result.resultPosition);
assert.equal(result.isUndefined, false);
assert.equal(result.snapshot.counters.property_get, 1);
assert.equal(result.snapshot.counters.type_is_undefined, 1);
assert.equal(result.snapshot.counters.stack_get, 3);
assert.equal(result.snapshot.counters.stack_push, 1);
assert.equal(fixture.get(result.resultPosition), targetValue);

let getterCount = 0;
const getterObject = {};
Object.defineProperty(getterObject, proxyTarget, {
    get() {
        ++getterCount;
        return targetValue;
    },
});
result = compoundGet(getterObject);
assert.equal(result.isUndefined, false);
assert.equal(fixture.get(result.resultPosition), targetValue);
assert.equal(getterCount, 1, "the compound operation must execute exactly one accessor getter");

let trapCount = 0;
const proxy = new Proxy({}, {
    get(target, property, receiver) {
        ++trapCount;
        return Reflect.get(target, property, receiver);
    },
});
result = compoundGet(proxy);
assert.equal(result.encodedResult, ~stackPositions.Undefined);
assert.equal(result.resultPosition, stackPositions.Undefined);
assert.equal(result.isUndefined, true);
assert.equal(trapCount, 1, "the compound operation must execute exactly one Proxy get trap");

const mapTarget = { mapTarget: true };
result = compoundGet(new Map([[proxyTarget, mapTarget]]));
assert.equal(result.isUndefined, false);
assert.equal(fixture.get(result.resultPosition), mapTarget, "Map lookup behavior must remain unchanged");

fixture.set(stackPositions.Undefined, "mutated fixed slot");
result = compoundGet({});
assert.equal(result.encodedResult, stackPositions.Undefined);
assert.equal(result.resultPosition, stackPositions.Undefined);
assert.equal(
    result.isUndefined,
    false,
    "the scalar tag must come from the live post-property stack read, not position comparison",
);
assert.equal(result.snapshot.counters.type_is_undefined, 1);
assert.equal(result.snapshot.counters.stack_get, 3);
fixture.set(stackPositions.Undefined, undefined);

fixture.set(stackPositions.Null, undefined);
result = compoundGet({ [proxyTarget]: null });
assert.equal(result.encodedResult, ~stackPositions.Null);
assert.equal(result.resultPosition, stackPositions.Null);
assert.equal(result.isUndefined, true, "undefined tagging must preserve a mutated nonzero fixed position");
fixture.set(stackPositions.Null, null);

fixture.mutateNextPropertyResult(undefined);
result = compoundGet({ [proxyTarget]: targetValue });
assert.ok(result.resultPosition > stackPositions.Error);
assert.equal(result.encodedResult, ~result.resultPosition);
assert.equal(result.isUndefined, true);
assert.equal(
    fixture.get(result.resultPosition),
    undefined,
    "the decoded Local must retain the exact slot changed after the authoritative property get",
);

for (const stackPosition of [0, 1, 2, 6, 8, 0x7fffffff]) {
    const encodedResult = fixture.encodeSyntheticUndefined(stackPosition);
    assert.equal(encodedResult, ~stackPosition, `undefined position ${stackPosition} must encode exactly`);
    assert.ok(encodedResult < 0);
    assert.equal(~encodedResult, stackPosition, `undefined position ${stackPosition} must round-trip exactly`);
}

const thrown = { originalThrownValue: true };
const throwingObject = {};
Object.defineProperty(throwingObject, proxyTarget, {
    get() {
        throw thrown;
    },
});
result = compoundGet(throwingObject);
assert.equal(result.encodedResult, stackPositions.Error);
assert.equal(result.resultPosition, stackPositions.Error);
assert.equal(result.isUndefined, false, "Error must remain untagged and bypass the undefined read");
assert.equal(result.snapshot.counters.property_get, 1);
assert.equal(result.snapshot.counters.type_is_undefined, 0);
assert.equal(result.snapshot.counters.stack_get, 3);
assert.equal(result.snapshot.counters.stack_set, 1);
assert.equal(fixture.get(stackPositions.Error), thrown, "the exact thrown value must remain in the Error slot");
fixture.set(stackPositions.Error, undefined);

assert.equal(bridgeErrorLogs.length, 1, "only the intentional throwing getter should log a bridge error");
assert.equal(profiler.set_enabled(0, false), true);

console.log("Web property/undefined compound regression tests passed.");
