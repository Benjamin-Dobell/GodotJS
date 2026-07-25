import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const typeConvertPath = join(moduleRoot, "bridge/jsb_type_convert.cpp");
const typedefPath = join(moduleRoot, "impl/web/jsb_web_typedef.h");
const interopPath = join(moduleRoot, "impl/web/jsb_web_interop.h");
const dataHeaderPath = join(moduleRoot, "impl/web/jsb_web_data.h");
const dataSourcePath = join(moduleRoot, "impl/web/jsb_web_data.cpp");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const typeConvertSource = readFileSync(typeConvertPath, "utf8");
const typedefSource = readFileSync(typedefPath, "utf8");
const interopSource = readFileSync(interopPath, "utf8");
const dataHeaderSource = readFileSync(dataHeaderPath, "utf8");
const dataSource = readFileSync(dataSourcePath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

function enumMembers(source, enumName) {
    const match = source.match(
        new RegExp(`(?:const\\s+)?enum(?:\\s+class)?\\s+${enumName}[^\\{]*\\{([\\s\\S]*?)\\}`),
    );
    assert.ok(match, `missing enum ${enumName}`);
    return match[1]
        .split(",")
        .map((entry) => entry.replace(/\/\/.*$/gm, "").trim())
        .filter(Boolean)
        .map((entry) => entry.split("=")[0].trim());
}

const expectedKinds = ["None", "Int32", "Number", "Num"];
assert.deepEqual(enumMembers(typedefSource, "WebNumericValueKind"), expectedKinds);
assert.deepEqual(enumMembers(monolithSource, "jsbb_NumericValueKind"), expectedKinds);
assert.match(
    interopSource,
    /JSBROWSER_API double jsbi_GetNumericValue\(jsb::impl::JSRuntime engine_id, jsb::impl::StackPosition stack_pos, int32_t\* o_kind\);/,
);
assert.equal(
    librarySource.match(/"jsbi_GetNumericValue": function/g)?.length ?? 0,
    1,
    "the Emscripten library must expose exactly one compound numeric forwarder",
);
assert.match(
    librarySource,
    /"jsbi_GetNumericValue": function \(engine_id, stack_pos, o_kind\) \{ return _jsbb_\.GetEngine\(engine_id\)\.GetNumericValue\(stack_pos, o_kind\); \}/,
);
assert.match(
    dataHeaderSource,
    /jsb::impl::WebNumericValueKind GetNumericValue\(double& r_value\) const;/,
);

const dataMethodSource = sourceBetween(
    dataSource,
    "jsb::impl::WebNumericValueKind Data::GetNumericValue",
    "bool Data::IsExternal",
);
assert.equal(dataMethodSource.match(/jsbi_GetNumericValue/g)?.length ?? 0, 1);
assert.match(dataMethodSource, /jsb_check\(kind >= 0 && kind < \(int32_t\) jsb::impl::WebNumericValueKind::Num\);/);

const monolithMethodSource = sourceBetween(
    monolithSource,
    "GetNumericValue(stack_pos: StackPosition, o_kind_ptr: Pointer): number",
    "NumberValue(stack_pos: StackPosition): number",
);
assert.equal(
    monolithMethodSource.match(/this\._stack\.GetValue\(stack_pos\)/g)?.length ?? 0,
    1,
    "the compound operation must read the live stack value once",
);
assert.match(monolithMethodSource, /jsbb_WebBridgeCounter\.ValueNumeric/);
assert.match(monolithMethodSource, /typeof val !== "number"/);
assert.match(monolithMethodSource, /Number\.isInteger\(val\)/);
assert.match(monolithMethodSource, /return val \| 0;/);
assert.match(monolithMethodSource, /return val;/);
assert.doesNotMatch(
    monolithMethodSource,
    /Number\(val\)|Boolean\(val\)|String\(val\)|valueOf|Symbol\.toPrimitive/,
    "classification must not coerce or invoke user code",
);
assert.match(monolithSource, /ValueNumeric,\s*ValueNumber,/);
assert.match(monolithSource, /"value_numeric",\s*"value_number",/);
assert.match(monolithSource, /const jsbb_WebBridgeProfileVersion = 2;/);

const conversionSource = sourceBetween(
    typeConvertSource,
    "bool TypeConvert::js_to_gd_var(v8::Isolate* isolate, const v8::Local<v8::Context>& context, const v8::Local<v8::Value>& p_jval, Variant& r_cvar)",
    "bool TypeConvert::can_convert_strict",
);
const numericBranchMatch = conversionSource.match(
    /#if JSB_WITH_WEB([\s\S]*?)#else([\s\S]*?)#endif/,
);
assert.ok(numericBranchMatch, "untyped conversion must isolate the compound operation to Web");
const [, webNumericSource, nativeNumericSource] = numericBranchMatch;
assert.equal(webNumericSource.match(/GetNumericValue/g)?.length ?? 0, 1);
assert.doesNotMatch(webNumericSource, /IsInt32|IsUint32|IsNumber|Int32::Value|Uint32::Value|Number::Value/);
assert.match(webNumericSource, /WebNumericValueKind::Int32/);
assert.match(webNumericSource, /WebNumericValueKind::Number/);
assert.match(
    nativeNumericSource,
    /IsInt32\(\)[\s\S]*As<v8::Int32>\(\)->Value\(\)[\s\S]*IsUint32\(\)[\s\S]*As<v8::Uint32>\(\)->Value\(\)[\s\S]*IsNumber\(\)[\s\S]*As<v8::Number>\(\)->Value\(\)/,
    "native V8/JSC/QuickJS conversion must retain the existing classification/fetch sequence",
);
assert.ok(
    conversionSource.indexOf("IsNullOrUndefined()") < conversionSource.indexOf("#if JSB_WITH_WEB")
        && conversionSource.indexOf("IsBoolean()") < conversionSource.indexOf("#if JSB_WITH_WEB"),
    "nullish and boolean early returns must remain ahead of numeric classification",
);
for (const fallback of ["IsString()", "IsArrayBuffer()", "IsObject()", "IsBigInt()"]) {
    assert.ok(
        conversionSource.indexOf("#if JSB_WITH_WEB") < conversionSource.indexOf(fallback),
        `${fallback} fallback must remain after numeric classification`,
    );
}
assert.match(
    conversionSource,
    /if \(maybe_target\.ToLocal\(&target\) && !target->IsUndefined\(\)\)\s*\{\s*return js_to_gd_var\(isolate, context, target, r_cvar\);\s*\}/,
    "proxy target recursion and error handling must remain unchanged",
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
    Set,
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
globalThis.__numericValueTest = {
    push(value) {
        return _jsbb_.GetEngine(0).stack.Push(value);
    },
    initializeI64() {
        _jsbb_["_i64"] = new BigInt64Array(wasmMemory.buffer);
    },
};
${librarySource}`,
    context,
    { filename: "godotjs-web-numeric-value-test.js" },
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
assert.equal(profiler.snapshot(0).version, 2, "the named counter schema must publish version 2");
const fixture = context.__numericValueTest;
fixture.initializeI64();
const kindPointer = 2048;
const bigintPointer = 2056;
const kind = {
    None: 0,
    Int32: 1,
    Number: 2,
};

function oldNumericResult(stackPosition) {
    if (bridge.jsbi_IsInt32(0, stackPosition)) {
        return { kind: kind.Int32, value: bridge.jsbi_Int32Value(0, stackPosition) };
    }
    if (bridge.jsbi_IsUint32(0, stackPosition)) {
        throw new Error("the duplicate Web Uint32 branch must remain unreachable");
    }
    if (bridge.jsbi_IsNumber(0, stackPosition)) {
        return { kind: kind.Number, value: bridge.jsbi_NumberValue(0, stackPosition) };
    }
    return { kind: kind.None, value: 0 };
}

function compoundNumericResult(stackPosition) {
    context.HEAP32[kindPointer >> 2] = -1;
    const value = bridge.jsbi_GetNumericValue(0, stackPosition, kindPointer);
    return {
        kind: context.HEAP32[kindPointer >> 2],
        value,
    };
}

function assertSameNumericResult(name, actual, expected) {
    assert.equal(actual.kind, expected.kind, `${name}: result kind`);
    assert.ok(
        Object.is(actual.value, expected.value),
        `${name}: expected ${String(expected.value)}, received ${String(actual.value)}`,
    );
}

const revoked = Proxy.revocable({}, {});
revoked.revoke();
let ordinaryProxyGets = 0;
const ordinaryProxy = new Proxy({}, {
    get(target, property, receiver) {
        ++ordinaryProxyGets;
        return Reflect.get(target, property, receiver);
    },
});
let throwingProxyGets = 0;
const throwingProxy = new Proxy({}, {
    get() {
        ++throwingProxyGets;
        throw new Error("throwing proxy trap");
    },
});
let coercionCalls = 0;
const coercionObject = {
    valueOf() {
        ++coercionCalls;
        return 7;
    },
    [Symbol.toPrimitive]() {
        ++coercionCalls;
        return 7;
    },
};

const cases = [
    ["zero", 0],
    ["negative zero", -0],
    ["int32 min", -2147483648],
    ["int32 max", 2147483647],
    ["below int32", -2147483649],
    ["uint32 start", 2147483648],
    ["uint32 max", 4294967295],
    ["above uint32", 4294967296],
    ["max safe integer", Number.MAX_SAFE_INTEGER],
    ["min safe integer", Number.MIN_SAFE_INTEGER],
    ["positive fraction", 1.5],
    ["negative fraction", -1.5],
    ["NaN", NaN],
    ["positive infinity", Infinity],
    ["negative infinity", -Infinity],
    ["bigint", 1n],
    ["string", "1"],
    ["empty string", ""],
    ["zero-length ArrayBuffer", new ArrayBuffer(0)],
    ["ArrayBuffer", new ArrayBuffer(4)],
    ["typed array", new Uint8Array(4)],
    ["null", null],
    ["undefined", undefined],
    ["ordinary object", {}],
    ["coercion object", coercionObject],
    ["function", function () {}],
    ["ordinary proxy", ordinaryProxy],
    ["throwing proxy", throwingProxy],
    ["revoked proxy", revoked.proxy],
    ["Error object", new Error("value")],
    ["symbol", Symbol("value")],
];

for (const [name, value] of cases) {
    const stackPosition = fixture.push(value);

    assert.equal(profiler.reset(0), true);
    const oldResult = oldNumericResult(stackPosition);

    assert.equal(profiler.reset(0), true);
    const newResult = compoundNumericResult(stackPosition);
    assertSameNumericResult(name, newResult, oldResult);

    const counters = profiler.snapshot(0).counters;
    assert.equal(counters.value_numeric, 1, `${name}: exactly one compound operation`);
    assert.equal(counters.stack_get, 1, `${name}: exactly one live stack read`);
    for (const counterName of [
        "type_is_int32",
        "type_is_uint32",
        "type_is_number",
        "value_int32",
        "value_uint32",
        "value_number",
    ]) {
        assert.equal(counters[counterName], 0, `${name}: ${counterName} must be bypassed`);
    }
    assert.equal(bridge.jsbi_HasError(0), false, `${name}: numeric classification must not set bridge error`);
}

assert.equal(coercionCalls, 0, "numeric classification must not invoke object coercion hooks");
assert.equal(ordinaryProxyGets, 0, "numeric classification must not invoke ordinary proxy traps");
assert.equal(throwingProxyGets, 0, "numeric classification must not invoke throwing proxy traps");

for (const [name, value] of [["null", null], ["undefined", undefined]]) {
    const stackPosition = fixture.push(value);
    assert.equal(profiler.reset(0), true);
    assert.equal(bridge.jsbi_IsNullOrUndefined(0, stackPosition), true, `${name}: nullish early return`);
    assert.equal(profiler.snapshot(0).counters.value_numeric, 0, `${name}: numeric operation is bypassed`);
}
for (const value of [true, false]) {
    const stackPosition = fixture.push(value);
    assert.equal(profiler.reset(0), true);
    assert.equal(bridge.jsbi_IsNullOrUndefined(0, stackPosition), false);
    assert.equal(bridge.jsbi_IsBoolean(0, stackPosition), true);
    assert.equal(profiler.snapshot(0).counters.value_numeric, 0, "boolean early return bypasses numeric operation");
}

for (const [name, value, expected] of [
    ["zero-length ArrayBuffer", new ArrayBuffer(0), true],
    ["ArrayBuffer", new ArrayBuffer(4), true],
    ["typed array", new Uint8Array(4), false],
]) {
    const stackPosition = fixture.push(value);
    assert.equal(compoundNumericResult(stackPosition).kind, kind.None, `${name}: numeric rejection`);
    assert.equal(bridge.jsbi_IsArrayBuffer(0, stackPosition), expected, `${name}: buffer fallback`);
}

for (const value of [1n, 9223372036854775807n, -9223372036854775808n]) {
    const stackPosition = fixture.push(value);
    assert.equal(compoundNumericResult(stackPosition).kind, kind.None, "bigint numeric rejection");
    assert.equal(bridge.jsbi_IsBigInt(0, stackPosition), true, "bigint fallback classification");
    assert.equal(bridge.jsbi_Int64Value(0, stackPosition, bigintPointer), true, "bigint fallback fetch");
    assert.equal(new BigInt64Array(memory)[bigintPointer >> 3], value, "bigint payload");
}

const proxyTargetKey = fixture.push(Symbol("ProxyTarget"));
const ordinaryProxyPosition = fixture.push(ordinaryProxy);
assert.equal(compoundNumericResult(ordinaryProxyPosition).kind, kind.None);
assert.equal(bridge.jsbi_GetProperty(0, ordinaryProxyPosition, proxyTargetKey), 0);
assert.equal(ordinaryProxyGets, 1, "ordinary proxy fallback invokes its getter exactly once");
assert.equal(bridge.jsbi_HasError(0), false);

for (const [name, proxy, getCount] of [
    ["throwing", throwingProxy, () => throwingProxyGets],
    ["revoked", revoked.proxy, () => 0],
]) {
    const stackPosition = fixture.push(proxy);
    assert.equal(compoundNumericResult(stackPosition).kind, kind.None, `${name}: numeric rejection`);
    assert.equal(bridge.jsbi_HasError(0), false, `${name}: no numeric bridge error`);
    assert.equal(bridge.jsbi_GetProperty(0, stackPosition, proxyTargetKey), 7, `${name}: proxy fallback error`);
    assert.equal(bridge.jsbi_HasError(0), true, `${name}: Error slot`);
    if (name === "throwing") {
        assert.equal(getCount(), 1, "throwing proxy getter count");
    }
    bridge.jsbi_StackSet(0, 7, 0);
    assert.equal(bridge.jsbi_HasError(0), false, `${name}: Error slot reset`);
}
assert.equal(bridgeErrorLogs.length, 2, "throwing and revoked proxy fallbacks must each report one bridge error");

assert.throws(
    () => bridge.jsbi_GetNumericValue(0, 999999, kindPointer),
    { name: "RangeError", message: "invalid stack position" },
    "invalid stack positions must retain the primary Stack.GetValue error",
);

assert.equal(profiler.set_enabled(0, false), true);
bridge.jsbi_FreeEngine(0);

console.log("GodotJS Web numeric value classification test: PASS");
