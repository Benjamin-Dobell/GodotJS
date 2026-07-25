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

const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

assert.throws(
    () => new WeakRef(null),
    TypeError,
    "the host WeakRef contract must reject null targets",
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

class StrictControlledWeakRef {
    static instances = [];
    static invalidTargets = [];

    constructor(target) {
        const type = typeof target;
        if (target === null || (type !== "object" && type !== "function")) {
            StrictControlledWeakRef.invalidTargets.push(target);
            throw new TypeError("WeakRef target must be an object or function");
        }

        this.target = target;
        StrictControlledWeakRef.instances.push(this);
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
    WeakRef: StrictControlledWeakRef,
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
globalThis.__globalNullTest = { runtime: _jsbb_ };
${librarySource}`,
    context,
    { filename: "godotjs-web-global-null-test.js" },
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

const engine = context.__globalNullTest.runtime.GetEngine(0);
const profiler = context.GodotJSBridgeProfiler;
assert.equal(profiler.set_enabled(0, true), true);

function addHandle(value, stackPosition) {
    const weakRefIndex = StrictControlledWeakRef.instances.length;
    let handle;

    engine.stack.EnterScope();
    try {
        const valuePosition = stackPosition ?? engine.stack.Push(value);
        assert.doesNotThrow(
            () => {
                handle = bridge.jsbi_handle_New(0, valuePosition);
            },
            `global allocation must provide WeakRef with a traceable target for ${String(value)}`,
        );
    } finally {
        engine.stack.ExitScope();
    }

    const token = StrictControlledWeakRef.instances[weakRefIndex];
    assert.ok(token, "global allocation must create one controlled WeakRef");
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
    assert.equal(Object.is(actual.value, expected), true, message);
}

class GlobalAbiHarness {
    constructor(value, stackPosition) {
        this.assign(value, stackPosition);
    }

    static take(handle, token) {
        const global = Object.create(GlobalAbiHarness.prototype);
        global.handle = handle;
        global.token = token;
        return global;
    }

    assign(value, stackPosition) {
        this.reset();
        const allocated = addHandle(value, stackPosition);
        this.handle = allocated.handle;
        this.token = allocated.token;
    }

    setWeak() {
        bridge.jsbi_handle_SetWeak(0, this.handle);
    }

    clearWeak() {
        bridge.jsbi_handle_ClearWeak(0, this.handle);
    }

    move() {
        const moved = GlobalAbiHarness.take(this.handle, this.token);
        this.handle = undefined;
        this.token = undefined;
        return moved;
    }

    reset() {
        if (this.handle === undefined) {
            return;
        }
        bridge.jsbi_handle_Reset(0, this.handle);
        this.handle = undefined;
        this.token = undefined;
    }
}

const baselineHandleCount = engine.handles.count;

assert.equal(engine.stack.GetValue(1), null, "the canonical null stack slot must retain null");
assert.equal(bridge.jsbi_IsNull(0, 1), true);
assert.equal(bridge.jsbi_IsUndefined(0, 1), false);
assert.equal(bridge.jsbi_stack_eq(0, 1, 1), true);
assert.equal(bridge.jsbi_stack_eq(0, 1, 0), false);

const firstNull = new GlobalAbiHarness(null, 1);
const secondNull = new GlobalAbiHarness(null);
assert.equal(typeof firstNull.token.target, "object", "null must be held through an object wrapper");
assert.equal(typeof secondNull.token.target, "object", "each null global must have a traceable target");
assert.notEqual(firstNull.token.target, secondNull.token.target, "distinct null globals must own distinct wrappers");
assertResolved(resolveHandle(firstNull.handle), null, "canonical null global round trip");
assertResolved(resolveHandle(secondNull.handle), null, "pushed null global round trip");
assert.equal(bridge.jsbi_handle_eq(0, firstNull.handle, secondNull.handle), true);

const unequalValues = [
    undefined,
    { kind: "object" },
    function callable() {},
    false,
    0,
    "",
    1n,
    Symbol("primitive"),
    new Number(0),
    new String(""),
    new Boolean(false),
];
const unequalGlobals = unequalValues.map((value) => new GlobalAbiHarness(value));
for (const [index, global] of unequalGlobals.entries()) {
    const value = unequalValues[index];
    const type = typeof value;
    if (type !== "object" && type !== "function") {
        assert.equal(typeof global.token.target, "object", "primitive globals must use traceable wrappers");
        assert.notEqual(global.token.target, value, "the WeakRef target must not be the primitive itself");
    }
    assert.equal(
        bridge.jsbi_handle_eq(0, firstNull.handle, global.handle),
        false,
        "null must remain strictly unequal to undefined, objects, functions, and boxed primitives",
    );
}

firstNull.setWeak();
assertResolved(resolveHandle(firstNull.handle), null, "live weak null global");
firstNull.clearWeak();
assertResolved(resolveHandle(firstNull.handle), null, "restored strong null global");
const movedNull = firstNull.move();
firstNull.reset();
assert.equal(engine.handles.count, baselineHandleCount + 2 + unequalGlobals.length);
assertResolved(resolveHandle(movedNull.handle), null, "moved null global");
movedNull.reset();

const collectedNull = new GlobalAbiHarness(null);
collectedNull.setWeak();
const collectedNullHandle = collectedNull.handle;
collectedNull.token.collect();
assert.equal(resolveHandle(collectedNullHandle).stackPosition, -1);
assert.equal(bridge.jsbi_handle_IsValid(0, collectedNullHandle), false);
collectedNull.reset();

const liveObjectValue = { state: "live" };
const liveObject = new GlobalAbiHarness(liveObjectValue);
assert.equal(liveObject.token.target, liveObjectValue, "traceable objects must not be wrapped");
liveObject.setWeak();
assertResolved(resolveHandle(liveObject.handle), liveObjectValue, "live weak object");
liveObject.reset();

const deadObject = new GlobalAbiHarness({ state: "dead" });
deadObject.setWeak();
const deadObjectHandle = deadObject.handle;
deadObject.token.collect();
assert.equal(resolveHandle(deadObjectHandle).stackPosition, -1);
assert.equal(bridge.jsbi_handle_IsValid(0, deadObjectHandle), false);
deadObject.reset();

const foreignRealm = vm.createContext({});
const foreignObject = vm.runInContext("({ realm: 'foreign' })", foreignRealm);
const foreignFunction = vm.runInContext("(function foreignCallable() {})", foreignRealm);
for (const value of [foreignObject, foreignFunction]) {
    const global = new GlobalAbiHarness(value);
    assert.equal(global.token.target, value, "cross-realm traceable values must not be wrapped");
    global.setWeak();
    assertResolved(resolveHandle(global.handle), value, "cross-realm weak value");
    global.reset();
}

for (const global of unequalGlobals) {
    global.reset();
}
secondNull.reset();
assert.equal(engine.handles.count, baselineHandleCount);
assert.deepEqual(StrictControlledWeakRef.invalidTargets, []);

const snapshot = profiler.snapshot(0);
assert.equal(snapshot.version, 3);
assert.equal(snapshot.counters.handle_new, 18);
assert.equal(snapshot.counters.handle_equals, 12);
assert.equal(snapshot.counters.handle_set_weak, 6);
assert.equal(snapshot.counters.handle_set_strong, 1);
assert.equal(snapshot.counters.handle_resolve_and_push, 10);
assert.equal(snapshot.counters.handle_collected, 2);
assert.equal(snapshot.counters.handle_reset, 18);
assert.equal(snapshot.gauges.handle_count, baselineHandleCount);

const releaseNull = new GlobalAbiHarness(null, 1);
assert.equal(engine.handles.count, baselineHandleCount + 1);
assertResolved(resolveHandle(releaseNull.handle), null, "null global before engine release");
bridge.jsbi_FreeEngine(0);
assert.equal(context.__globalNullTest.runtime.GetEngine(0), undefined);

const traceableSource = sourceBetween(
    monolithSource,
    "function jsbb_IsTraceable(o: any): boolean {",
    "function jsbb_ensure(condition: boolean)",
);
assert.match(
    traceableSource,
    /return o !== null && \(type === "function" \|\| type === "object"\);/,
    "traceability must positively require a non-null object or function",
);

console.log("GodotJS Web null Global traceability test: PASS");
