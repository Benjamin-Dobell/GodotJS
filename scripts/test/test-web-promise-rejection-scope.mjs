import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const environmentPath = join(moduleRoot, "bridge/jsb_environment.cpp");
const webGlobalInitPath = join(moduleRoot, "impl/web/jsb_web_global_init.cpp");
const webPromisePath = join(moduleRoot, "impl/web/jsb_web_promise_reject.cpp");
const webHelperPath = join(moduleRoot, "impl/web/jsb_web_helper.h");
const webPrimitivePath = join(moduleRoot, "impl/web/jsb_web_primitive.cpp");
const quickjsIsolatePath = join(moduleRoot, "impl/quickjs/jsb_quickjs_isolate.cpp");
const jscIsolatePath = join(moduleRoot, "impl/jsc/jsb_jsc_isolate.h");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const environmentSource = readFileSync(environmentPath, "utf8");
const webGlobalInitSource = readFileSync(webGlobalInitPath, "utf8");
const webPromiseSource = readFileSync(webPromisePath, "utf8");
const webHelperSource = readFileSync(webHelperPath, "utf8");
const webPrimitiveSource = readFileSync(webPrimitivePath, "utf8");
const quickjsIsolateSource = readFileSync(quickjsIsolatePath, "utf8");
const jscIsolateSource = readFileSync(jscIsolatePath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

const hostTrackerSource = sourceBetween(
    monolithSource,
    "SetHostPromiseRejectionTracker(cb: FunctionPointer, data: Pointer)",
    "static GetStringHash",
);
assert.match(
    hostTrackerSource,
    /stack\.EnterScope\(\);[\s\S]*stack\.Push\(promise\), stack\.Push\(reason\)[\s\S]*catch \(err\)[\s\S]*jsbb_console\.error\("unexpected error", err\);[\s\S]*stack\.ExitScope\(\);/,
    "the JavaScript host listener must retain its outer scope and error handling",
);

const nativeRejectSource = sourceBetween(
    webGlobalInitSource,
    "JSNATIVE_API EMSCRIPTEN_KEEPALIVE void jsni_unhandled_rejection",
    "JSNATIVE_API EMSCRIPTEN_KEEPALIVE void jsni_gc_callback",
);
assert.match(nativeRejectSource, /v8::PromiseRejectMessage message\(/);
assert.match(nativeRejectSource, /cb\(message\);/);

assert.match(
    webPromiseSource,
    /return Local<Promise>\(Data\(isolate_, promise_sp_\)\);/,
    "Web rejection messages must remain views over the outer promise position",
);
assert.match(
    webPromiseSource,
    /return Local<Value>\(Data\(isolate_, reason_sp_\)\);/,
    "Web rejection messages must remain views over the outer reason position",
);
assert.match(
    environmentSource,
    /to_string_without_side_effect\(isolate, message\.GetValue\(\)\)/,
    "the fixed environment callback must retain detail-string conversion",
);
assert.match(
    webHelperSource,
    /p_val->ToDetailString\(isolate->GetCurrentContext\(\)\)/,
);
assert.match(
    webPrimitiveSource,
    /Value::ToDetailString[\s\S]*jsbi_ToString\(isolate_->rt\(\), stack_pos_\)/,
);
assert.match(
    quickjsIsolateSource,
    /_promise_rejection_tracker[\s\S]*HandleScope handle_scope\(isolate\);[\s\S]*isolate->push_copy\(promise\)[\s\S]*isolate->push_copy\(reason\)/,
    "QuickJS must retain its callback-local ownership scope",
);
assert.match(
    jscIsolateSource,
    /SetPromiseRejectCallback[\s\S]*SetPromiseRejectCallback is not supported/,
    "JavaScriptCore promise rejection behavior must remain untouched",
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

const memory = new ArrayBuffer(16 * 1024);
const listeners = new Map();
const consoleErrors = [];
const consoleWarnings = [];
let nextAllocation = 1024;
let engine;
let modelInnerScope = true;
let nativeCallback;

const testConsole = {
    assert: console.assert,
    error: (...args) => {
        consoleErrors.push(args);
        if (args[0] === "[jsbb]" && args[1] === "unexpected error") {
            nativeCallback?.onCaughtError?.();
        }
    },
    log: () => {},
    warn: (...args) => consoleWarnings.push(args),
};

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
    Symbol,
    Uint8Array,
    WeakMap,
    WeakRef,
    WebAssembly,
    addEventListener: (name, listener) => listeners.set(name, listener),
    console: testConsole,
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
globalThis.__promiseScopeTest = {
    runtime: _jsbb_,
    stackBase: jsbb_StackPos,
};
${librarySource}`,
    context,
    { filename: "godotjs-web-promise-rejection-scope-test.js" },
);

const bridge = context.LibraryManager.library;
const callFunction = (_opaque, callback, isConstructCall, basePosition, argc) => {
    callback(isConstructCall, basePosition, argc);
};
const unhandledRejection = (_opaque, callback, promisePosition, reasonPosition) => {
    if (modelInnerScope) {
        engine.stack.EnterScope();
    }
    try {
        callback(promisePosition, reasonPosition);
    } finally {
        if (modelInnerScope) {
            engine.stack.ExitScope();
        }
    }
};

bridge.jsbi_init(
    () => {},
    unhandledRejection,
    callFunction,
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

engine = context.__promiseScopeTest.runtime.GetEngine(0);
const stackBase = context.__promiseScopeTest.stackBase;
const profiler = context.GodotJSBridgeProfiler;
assert.equal(profiler.set_enabled(0, true), true);

let reentryFunction;
engine.stack.EnterScope();
try {
    const functionPosition = bridge.jsbi_NewCFunction(
        0,
        (_isConstructCall, basePosition, argc) => {
            assert.equal(argc, 1);
            nativeCallback.reentries.push({
                argument: engine.stack.GetValue(basePosition + 4),
                basePosition,
                depth: engine.stack.depth,
                position: engine.stack.pos,
            });
            engine.stack.SetValue(basePosition, "reentered");
        },
        stackBase.Undefined,
        0,
    );
    reentryFunction = engine.stack.GetValue(functionPosition);
} finally {
    engine.stack.ExitScope();
}

bridge.jsbi_SetHostPromiseRejectionTracker(
    0,
    (promisePosition, reasonPosition) => nativeCallback.run(promisePosition, reasonPosition),
);
const rejectionListener = listeners.get("unhandledrejection");
assert.equal(typeof rejectionListener, "function");

function resetScenario(innerScope) {
    modelInnerScope = innerScope;
    engine.stack.SetValue(stackBase.Error, undefined);
    consoleErrors.length = 0;
    consoleWarnings.length = 0;
    assert.equal(profiler.reset(0), true);
}

function assertClean(expectedError) {
    assert.equal(engine.stack.GetValue(stackBase.Error), expectedError, "event return changed Error slot");
    const snapshot = profiler.snapshot(0);
    assert.equal(engine.stack.GetValue(stackBase.Error), expectedError, "profile snapshot changed Error slot");
    assert.equal(snapshot.gauges.stack_position, stackBase._Num);
    assert.equal(snapshot.gauges.stack_depth, 0);
    assert.equal(engine.stack.values.includes(nativeCallback.promise), false);
    assert.equal(engine.stack.GetValue(stackBase.Error), expectedError, "promise retention check changed Error slot");
    assert.equal(engine.stack.values.includes(nativeCallback.reason), false);
    assert.equal(engine.stack.GetValue(stackBase.Error), expectedError, "reason retention check changed Error slot");
    return snapshot;
}

function runNormal(innerScope) {
    resetScenario(innerScope);
    const errorSentinel = { scenario: "normal-error-slot" };

    const promise = { scenario: "normal-promise" };
    const reason = {
        [Symbol.toPrimitive]() {
            nativeCallback.coercionPositions.push({
                depth: engine.stack.depth,
                position: engine.stack.pos,
            });
            assert.equal(reentryFunction("coercion"), "reentered");
            return "normal-reason";
        },
    };

    nativeCallback = {
        promise,
        reason,
        coercionPositions: [],
        reentries: [],
        run(promisePosition, reasonPosition) {
            assert.equal(engine.stack.GetValue(promisePosition), promise);
            assert.equal(engine.stack.GetValue(reasonPosition), reason);
            assert.equal(promisePosition, stackBase._Num);
            assert.equal(reasonPosition, stackBase._Num + 1);

            const detailPosition = bridge.jsbi_ToString(0, reasonPosition);
            assert.equal(detailPosition, stackBase._Num + 2);
            assert.equal(engine.stack.GetValue(detailPosition), "normal-reason");
            assert.notEqual(bridge.jsbi_ToCStringLen(0, 16, detailPosition), 0);
            assert.equal(reentryFunction("after-push"), "reentered");
            engine.stack.SetValue(stackBase.Error, errorSentinel);
        },
    };

    rejectionListener({ promise, reason });
    assert.equal(consoleWarnings.length, 1);
    assert.equal(consoleErrors.length, 0);
    const snapshot = assertClean(errorSentinel);
    return {
        counters: snapshot.counters,
        peakDepth: snapshot.gauges.peak_stack_depth,
        peakPosition: snapshot.gauges.peak_stack_position,
        coercionPositions: nativeCallback.coercionPositions,
        reentries: nativeCallback.reentries,
    };
}

function runThrow(innerScope) {
    resetScenario(innerScope);
    const errorSentinel = { scenario: "throw-error-slot" };

    const promise = { scenario: "throw-promise" };
    const thrown = new Error("coercion failed");
    const reason = {
        [Symbol.toPrimitive]() {
            nativeCallback.coercionPositions.push({
                depth: engine.stack.depth,
                position: engine.stack.pos,
            });
            assert.equal(reentryFunction("coercion-before-throw"), "reentered");
            throw thrown;
        },
    };

    nativeCallback = {
        promise,
        reason,
        coercionPositions: [],
        reentries: [],
        caughtPositions: [],
        onCaughtError() {
            this.caughtPositions.push({
                depth: engine.stack.depth,
                position: engine.stack.pos,
            });
            assert.equal(reentryFunction("catch-log"), "reentered");
            engine.stack.SetValue(stackBase.Error, errorSentinel);
        },
        run(_promisePosition, reasonPosition) {
            bridge.jsbi_ToString(0, reasonPosition);
        },
    };

    rejectionListener({ promise, reason });
    assert.equal(consoleWarnings.length, 1);
    assert.equal(consoleErrors.length, 1);
    assert.equal(consoleErrors[0][2], thrown);
    const snapshot = assertClean(errorSentinel);
    return {
        counters: snapshot.counters,
        peakDepth: snapshot.gauges.peak_stack_depth,
        peakPosition: snapshot.gauges.peak_stack_position,
        coercionPositions: nativeCallback.coercionPositions,
        reentries: nativeCallback.reentries,
        caughtPositions: nativeCallback.caughtPositions,
    };
}

function compareScenarios(baseline, coalesced, expectedPeakPosition) {
    assert.equal(baseline.counters.scope_enter, 4);
    assert.equal(baseline.counters.scope_exit, 4);
    assert.equal(coalesced.counters.scope_enter, 3);
    assert.equal(coalesced.counters.scope_exit, 3);
    assert.equal(baseline.counters.scope_enter, coalesced.counters.scope_enter + 1);
    assert.equal(baseline.counters.scope_exit, coalesced.counters.scope_exit + 1);
    assert.equal(baseline.peakDepth, coalesced.peakDepth + 1);
    assert.equal(baseline.peakPosition, expectedPeakPosition);
    assert.equal(coalesced.peakPosition, expectedPeakPosition);
    assert.equal(baseline.counters.stack_push, coalesced.counters.stack_push);
    assert.equal(baseline.counters.stack_get, coalesced.counters.stack_get);
    assert.deepEqual(
        baseline.coercionPositions.map(({ position }) => position),
        coalesced.coercionPositions.map(({ position }) => position),
    );
    assert.deepEqual(
        baseline.coercionPositions.map(({ depth }) => depth),
        coalesced.coercionPositions.map(({ depth }) => depth + 1),
    );
    assert.deepEqual(
        baseline.reentries.map(({ argument, basePosition, position }) => ({ argument, basePosition, position })),
        coalesced.reentries.map(({ argument, basePosition, position }) => ({ argument, basePosition, position })),
    );
    assert.deepEqual(
        baseline.reentries
            .filter(({ argument }) => argument !== "catch-log")
            .map(({ depth }) => depth),
        coalesced.reentries
            .filter(({ argument }) => argument !== "catch-log")
            .map(({ depth }) => depth + 1),
    );
    if (baseline.caughtPositions) {
        assert.deepEqual(baseline.caughtPositions, coalesced.caughtPositions);
        assert.deepEqual(baseline.caughtPositions, [{
            depth: 1,
            position: stackBase._Num + 2,
        }]);
    }
}

const baselineNormal = runNormal(true);
const coalescedNormal = runNormal(false);
compareScenarios(baselineNormal, coalescedNormal, stackBase._Num + 8);

const baselineThrow = runThrow(true);
const coalescedThrow = runThrow(false);
compareScenarios(baselineThrow, coalescedThrow, stackBase._Num + 7);

assert.doesNotMatch(
    nativeRejectSource,
    /v8::HandleScope/,
    "RED: Web host rejection callback still enters a nested handle scope",
);

console.log("GodotJS Web promise rejection scope test: PASS");
