import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const helperPath = join(moduleRoot, "impl/web/jsb_web_helper.h");
const typeConvertPath = join(moduleRoot, "bridge/jsb_type_convert.cpp");
const classInfoPath = join(moduleRoot, "bridge/jsb_class_info.h");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const helperSource = readFileSync(helperPath, "utf8");
const typeConvertSource = readFileSync(typeConvertPath, "utf8");
const classInfoSource = readFileSync(classInfoPath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

const helperFunctionSource = sourceBetween(
    helperSource,
    "static bool get_internal_fields(",
    "static PackedByteArray to_packed_byte_array",
);
assert.equal(
    helperFunctionSource.match(/jsbi_GetOpaque/g)?.length ?? 0,
    1,
    "the grouped Web reader must cross GetOpaque exactly once",
);
assert.match(helperFunctionSource, /if \(!index\)\s*\{\s*return false;\s*\}/);
assert.match(helperFunctionSource, /isolate->get_internal_data\(index\)/);
assert.match(
    helperFunctionSource,
    /if \(!data \|\| data->internal_field_count != p_expected_field_count\)\s*\{\s*return false;\s*\}/,
    "missing data and the wrong internal-field layout must reject without reading slots",
);
assert.match(
    helperFunctionSource,
    /for \(int index = 0; index < p_expected_field_count; \+\+index\)\s*\{\s*r_fields\[index\] = data->internal_fields\[index\];\s*\}/,
    "the helper must synchronously copy the validated internal fields",
);
assert.doesNotMatch(
    helperFunctionSource,
    /\.escape\(\)|return data;/,
    "the scoped internal-data pointer must not escape the helper",
);

const conversionSource = sourceBetween(
    typeConvertSource,
    "bool TypeConvert::js_to_gd_obj(",
    "\n    }\n\n}",
);
assert.match(
    conversionSource,
    /if \(maybe_target\.ToLocal\(&target\) && !target->IsUndefined\(\)\)\s*\{\s*return js_to_gd_obj\(isolate, context, target, r_godot_obj\);\s*\}/,
    "proxy-target recursion and its existing MaybeLocal behavior must remain before direct conversion",
);
assert.ok(
    conversionSource.indexOf("return js_to_gd_obj(isolate, context, target, r_godot_obj);")
        < conversionSource.indexOf("#if JSB_WITH_WEB"),
    "proxy lookup, recursion, and exception propagation must remain ahead of the direct Web fast path",
);

const webBranchMatch = conversionSource.match(
    /#if JSB_WITH_WEB([\s\S]*?)#else([\s\S]*?)#endif/,
);
assert.ok(webBranchMatch, "direct conversion must isolate the grouped reader behind JSB_WITH_WEB");
const [, webBranchSource, nativeBranchSource] = webBranchMatch;
assert.equal(
    webBranchSource.match(/impl::Helper::get_internal_fields/g)?.length ?? 0,
    1,
    "direct Web conversion must call the grouped reader exactly once",
);
assert.doesNotMatch(
    webBranchSource,
    /InternalFieldCount\(|GetAlignedPointerFromInternalField\(/,
    "the Web branch must not repeat opaque-backed object accessors",
);
assert.match(webBranchSource, /internal_fields\[IF_ClassType\]/);
assert.match(webBranchSource, /void\* pointer = internal_fields\[IF_Pointer\];/);
assert.match(
    nativeBranchSource,
    /!TypeConvert::is_object\(self\)[\s\S]*GetAlignedPointerFromInternalField\(IF_ClassType\)[\s\S]*GetAlignedPointerFromInternalField\(IF_Pointer\)/,
    "the native V8/JSC/QuickJS path must retain its existing accessors",
);
assert.match(
    conversionSource,
    /r_godot_obj = Environment::wrap\(isolate\)->verify_object\(pointer\) \? \(Object\*\) pointer : nullptr;\s*return true;/,
    "live-object verification and the true-plus-null stale-object contract must remain shared",
);

const godotObjectType = Number(
    classInfoSource.match(/GodotObject\s*=\s*(\d+)/)?.[1],
);
assert.equal(godotObjectType, 36, "the counter model must use the source-defined Godot object tag");

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
globalThis.__boundObjectConversionTest = {
    push(value) {
        return _jsbb_.GetEngine(0).stack.Push(value);
    },
    pushWithOpaque(value, opaque) {
        value[jsbb_opaque] = opaque;
        return _jsbb_.GetEngine(0).stack.Push(value);
    },
    makeExternal(pointer) {
        return new jsbb_External(pointer);
    },
};
${librarySource}`,
    context,
    { filename: "godotjs-web-bound-object-conversion-test.js" },
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
const fixture = context.__boundObjectConversionTest;
const records = new Map();
const livePointers = new Set([0x1000, 0x2000]);

function directConversion(stackPosition) {
    const opaque = bridge.jsbi_GetOpaque(0, stackPosition);
    if (opaque === 0) {
        return { converted: false, object: undefined };
    }

    const data = records.get(opaque);
    assert.ok(data, "nonzero test opaque IDs model existing checked InternalData records");
    if (data.internalFieldCount !== 2) {
        return { converted: false, object: undefined };
    }

    const [pointer, classType] = data.internalFields;
    if (classType !== godotObjectType) {
        return { converted: false, object: undefined };
    }

    return {
        converted: true,
        object: pointer !== 0 && livePointers.has(pointer) ? pointer : null,
    };
}

function runObjectCase(name, value, record, expected) {
    if (record) {
        records.set(record.opaque, {
            internalFieldCount: record.internalFieldCount,
            internalFields: record.internalFields,
        });
    }
    const stackPosition = record
        ? fixture.pushWithOpaque(value, record.opaque)
        : fixture.push(value);
    assert.equal(profiler.reset(0), true);
    assert.deepEqual(
        directConversion(stackPosition),
        expected.result,
        `${name}: conversion result`,
    );
    assert.equal(
        profiler.snapshot(0).counters.get_opaque,
        expected.opaqueReads,
        `${name}: opaque-read count`,
    );
}

for (const [name, value] of [
    ["undefined", undefined],
    ["null", null],
    ["boolean", true],
    ["number", 17],
    ["string", "not bound"],
]) {
    const stackPosition = fixture.push(value);
    assert.equal(profiler.reset(0), true);
    const isObject = value !== null && (typeof value === "object" || typeof value === "function");
    const result = isObject
        ? directConversion(stackPosition)
        : { converted: false, object: undefined };
    assert.deepEqual(result, { converted: false, object: undefined }, `${name}: rejection`);
    assert.equal(profiler.snapshot(0).counters.get_opaque, 0, `${name}: no opaque read`);
}

runObjectCase(
    "plain object",
    {},
    undefined,
    { result: { converted: false, object: undefined }, opaqueReads: 1 },
);
runObjectCase(
    "Web external",
    fixture.makeExternal(0x1000),
    undefined,
    { result: { converted: false, object: undefined }, opaqueReads: 1 },
);
runObjectCase(
    "wrong internal-field count",
    {},
    { opaque: 101, internalFieldCount: 1, internalFields: [0x1000, godotObjectType] },
    { result: { converted: false, object: undefined }, opaqueReads: 1 },
);
runObjectCase(
    "wrong native class",
    {},
    { opaque: 102, internalFieldCount: 2, internalFields: [0x1000, 38] },
    { result: { converted: false, object: undefined }, opaqueReads: 1 },
);
runObjectCase(
    "live Godot object",
    {},
    { opaque: 103, internalFieldCount: 2, internalFields: [0x1000, godotObjectType] },
    { result: { converted: true, object: 0x1000 }, opaqueReads: 1 },
);
runObjectCase(
    "stale wrapper with cleared pointer",
    {},
    { opaque: 104, internalFieldCount: 2, internalFields: [0, godotObjectType] },
    { result: { converted: true, object: null }, opaqueReads: 1 },
);
runObjectCase(
    "deleted object absent from registry",
    {},
    { opaque: 105, internalFieldCount: 2, internalFields: [0x3000, godotObjectType] },
    { result: { converted: true, object: null }, opaqueReads: 1 },
);
const inheritedWrapper = Object.create({ inheritedGodotMethod: true });
runObjectCase(
    "inherited Godot wrapper",
    inheritedWrapper,
    { opaque: 106, internalFieldCount: 2, internalFields: [0x2000, godotObjectType] },
    { result: { converted: true, object: 0x2000 }, opaqueReads: 1 },
);

assert.equal(profiler.set_enabled(0, false), true);
bridge.jsbi_FreeEngine(0);

console.log("GodotJS Web bound-object conversion test: PASS");
