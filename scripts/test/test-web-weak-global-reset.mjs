import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const globalHeaderPath = join(moduleRoot, "impl/web/jsb_web_handle.h");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const libraryPath = join(moduleRoot, "impl/web/js/library_godotjs_jsbi.js");

const globalHeaderSource = readFileSync(globalHeaderPath, "utf8");
const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

const globalSource = sourceBetween(
    globalHeaderSource,
    "template <typename T>\n    class Global\n",
    "template <typename T>\n    template <typename S>\n",
);
assert.match(globalSource, /~Global\(\) \{ Reset\(\); \}/, "destruction must delegate to Reset");

const moveConstructorSource = sourceBetween(
    globalSource,
    "Global(Global&& other) noexcept",
    "template <typename S>\n        Global& operator=",
);
assert.match(moveConstructorSource, /isolate_ = other\.isolate_;/);
assert.match(moveConstructorSource, /weak_type_ = other\.weak_type_;/);
assert.match(moveConstructorSource, /value_ = other\.value_;/);
assert.match(moveConstructorSource, /other\._clear\(\);/);

const moveAssignmentSource = sourceBetween(
    globalSource,
    "Global& operator=(Global<S>&& other)",
    "void Reset()",
);
assert.match(moveAssignmentSource, /Reset\(\);/);
assert.match(moveAssignmentSource, /if \(!other\.IsEmpty\(\)\)/);
assert.match(moveAssignmentSource, /value_ = other\.value_;/);
assert.match(moveAssignmentSource, /other\._clear\(\);/);

const resetSource = sourceBetween(
    globalSource,
    "void Reset()",
    "void Reset(Isolate* isolate, Local<T> value)",
);
const plainWeakResetSource = sourceBetween(
    resetSource,
    "case WeakType::kWeak:",
    "case WeakType::kWeakCallback:",
);
assert.equal(
    plainWeakResetSource.match(
        /jsbi_handle_Reset\(jsb::impl::Broker::get_engine\(isolate_\), value_\);/g,
    )?.length ?? 0,
    1,
    "plain-weak reset must remove its browser handle entry before discarding the ID",
);
assert.doesNotMatch(
    plainWeakResetSource,
    /is_alive\(\)|SetWeakCallback/,
    "plain-weak reset needs neither a liveness probe nor callback mutation",
);

const callbackWeakResetSource = sourceBetween(
    resetSource,
    "case WeakType::kWeakCallback:",
    "default:",
);
assert.match(callbackWeakResetSource, /if \(is_alive\(\)\)/);
assert.match(callbackWeakResetSource, /Broker::SetWeakCallback\(isolate_, value_, nullptr, nullptr\);/);
assert.match(callbackWeakResetSource, /jsbi_handle_Reset\(jsb::impl::Broker::get_engine\(isolate_\), value_\);/);

const replacementResetSource = sourceBetween(
    globalSource,
    "void Reset(Isolate* isolate, Local<T> value)",
    "void Reset(Isolate* isolate, const Global& value)",
);
assert.match(
    replacementResetSource,
    /\{\s*Reset\(\);/,
    "replacing a Global must release its prior handle before allocation",
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
globalThis.__weakGlobalResetTest = { runtime: _jsbb_ };
${librarySource}`,
    context,
    { filename: "godotjs-web-weak-global-reset-test.js" },
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

const engine = context.__weakGlobalResetTest.runtime.GetEngine(0);

class PlainWeakGlobalAbiHarness {
    constructor(value) {
        this.assign(value);
    }

    static take(handle, token) {
        const global = Object.create(PlainWeakGlobalAbiHarness.prototype);
        global.handle = handle;
        global.token = token;
        return global;
    }

    assign(value) {
        this.reset();
        const weakRefCount = ControlledWeakRef.instances.length;
        engine.stack.EnterScope();
        try {
            this.handle = bridge.jsbi_handle_New(0, engine.stack.Push(value));
        } finally {
            engine.stack.ExitScope();
        }
        assert.equal(ControlledWeakRef.instances.length, weakRefCount + 1);
        this.token = ControlledWeakRef.instances.at(-1);
        bridge.jsbi_handle_SetWeak(0, this.handle);
    }

    move() {
        const moved = PlainWeakGlobalAbiHarness.take(this.handle, this.token);
        this.handle = undefined;
        this.token = undefined;
        return moved;
    }

    reset() {
        if (this.handle === undefined) {
            return;
        }
        bridge.jsbi_handle_Reset(0, this.handle);
        this.token.collect();
        this.handle = undefined;
        this.token = undefined;
    }
}

const baselineHandleCount = engine.handles.count;

const liveGlobal = new PlainWeakGlobalAbiHarness({ state: "live" });
const liveHandle = liveGlobal.handle;
assert.equal(engine.handles.count, baselineHandleCount + 1);
assert.equal(bridge.jsbi_handle_IsValid(0, liveHandle), true);
liveGlobal.reset();
assert.equal(engine.handles.count, baselineHandleCount);
assert.equal(bridge.jsbi_handle_IsValid(0, liveHandle), false);
liveGlobal.reset();
assert.equal(engine.handles.count, baselineHandleCount, "repeated reset must be a no-op");

const deadGlobal = new PlainWeakGlobalAbiHarness({ state: "dead" });
const deadHandle = deadGlobal.handle;
deadGlobal.token.collect();
assert.equal(engine.handles.count, baselineHandleCount + 1);
deadGlobal.reset();
assert.equal(engine.handles.count, baselineHandleCount);
assert.equal(bridge.jsbi_handle_IsValid(0, deadHandle), false);

const moveSource = new PlainWeakGlobalAbiHarness({ state: "moved" });
const movedHandle = moveSource.handle;
const moveDestination = moveSource.move();
moveSource.reset();
assert.equal(engine.handles.count, baselineHandleCount + 1, "moved-from reset must not release ownership");
assert.equal(bridge.jsbi_handle_IsValid(0, movedHandle), true);
moveDestination.reset();
assert.equal(engine.handles.count, baselineHandleCount);

const deadMoveSource = new PlainWeakGlobalAbiHarness({ state: "dead-moved" });
deadMoveSource.token.collect();
const deadMoveDestination = deadMoveSource.move();
deadMoveSource.reset();
assert.equal(engine.handles.count, baselineHandleCount + 1);
deadMoveDestination.reset();
assert.equal(engine.handles.count, baselineHandleCount);

const replacement = new PlainWeakGlobalAbiHarness({ generation: 1 });
const replacedHandle = replacement.handle;
replacement.assign({ generation: 2 });
assert.equal(engine.handles.count, baselineHandleCount + 1);
assert.equal(bridge.jsbi_handle_IsValid(0, replacedHandle), false);
replacement.reset();
assert.equal(engine.handles.count, baselineHandleCount);

const churnCycles = 4096;
for (let cycle = 0; cycle < churnCycles; ++cycle) {
    let owner = new PlainWeakGlobalAbiHarness({ cycle });
    if ((cycle & 1) === 0) {
        owner.token.collect();
    }
    if (cycle % 3 === 0) {
        owner = owner.move();
    }
    owner.reset();
    assert.equal(engine.handles.count, baselineHandleCount);
}

bridge.jsbi_FreeEngine(0);

console.log("GodotJS Web plain-weak Global reset test: PASS");
