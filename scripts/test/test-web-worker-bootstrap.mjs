import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

import ts from "../../impl/web/bridge/node_modules/typescript/lib/typescript.js";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const monolithPath = join(moduleRoot, "impl/web/bridge/src/monolith.ts");
const monolithSource = readFileSync(monolithPath, "utf8");

function sourceBetween(source, start, end) {
    const startIndex = source.indexOf(start);
    assert.notEqual(startIndex, -1, `missing source boundary: ${start}`);

    const endIndex = source.indexOf(end, startIndex + start.length);
    assert.notEqual(endIndex, -1, `missing source boundary: ${end}`);

    return source.slice(startIndex, endIndex);
}

function assertWorkerBootstrapSourceContract(source) {
    assert.equal(
        source.match(/function getPThreadRuntime\s*\(/g)?.length ?? 0,
        1,
        "the bridge must resolve PThread lazily instead of capturing it during module evaluation",
    );
    assert.doesNotMatch(
        source,
        /\bjsbb_pthread_runtime\b/,
        "the bridge must not retain a module-load PThread snapshot",
    );
    assert.equal(
        source.match(/\bgetPThreadRuntime\s*\(/g)?.length ?? 0,
        3,
        "the lazy PThread lookup must remain limited to its helper and two runtime consumers",
    );

    const resetSource = sourceBetween(
        source,
        "private static reset_pthread_runtime_state(): void",
        "static NewEngine(",
    );
    assert.match(
        resetSource,
        /\bJSWorkerParent\s*=\s*undefined\s*;/,
        "pthread runtime reset must discard the cached logical worker parent",
    );

    const postSource = sourceBetween(
        source,
        "static post_message_to_worker(",
        "private static install_pthread_worker_listener_hook(): void",
    );
    assert.match(
        postSource,
        /const worker = getPThreadRuntime\(\)\?\.pthreads\[pthread_id\];/,
        "worker posting must resolve the current PThread runtime",
    );

    const listenerSource = sourceBetween(
        source,
        "private static install_pthread_worker_listener_hook(): void",
        "private static dispatch_direct_worker_message(",
    );
    assert.match(listenerSource, /const pthread_runtime = getPThreadRuntime\(\);/);
    assert.match(listenerSource, /if \(jsbb_is_pthread \|\| !pthread_runtime\)/);
    assert.match(listenerSource, /const original_get_new_worker = pthread_runtime\.getNewWorker;/);
    assert.match(listenerSource, /original_get_new_worker\.call\(pthread_runtime\)/);
    assert.match(listenerSource, /pthread_runtime\.getNewWorker = wrapped_get_new_worker;/);
}

function transpileMonolith(source, label) {
    const transpiled = ts.transpileModule(source, {
        compilerOptions: {
            module: ts.ModuleKind.None,
            target: ts.ScriptTarget.ES2021,
            ignoreDeprecations: "6.0",
        },
        fileName: `${monolithPath}:${label}`,
        reportDiagnostics: true,
    });
    assert.deepEqual(
        transpiled.diagnostics?.map((diagnostic) => diagnostic.messageText) ?? [],
        [],
        `${label} monolith transpilation must not report diagnostics`,
    );
    return transpiled.outputText;
}

function evaluateMonolith(source, label, isPthread, requireWorkerModule) {
    const memory = new ArrayBuffer(16 * 1024);
    let nextAllocation = 1024;
    const context = vm.createContext({
        ArrayBuffer,
        BigInt64Array,
        ENVIRONMENT_IS_PTHREAD: isPthread,
        FinalizationRegistry,
        Float64Array,
        HEAP8: new Int8Array(memory),
        HEAP32: new Int32Array(memory),
        HEAPU8: new Uint8Array(memory),
        Map,
        Module: undefined,
        PThread: undefined,
        Promise,
        Set,
        Uint8Array,
        WeakMap,
        WeakRef,
        WebAssembly,
        addEventListener: () => {},
        console,
        globalThis: undefined,
        require: requireWorkerModule,
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
        `${transpileMonolith(source, label)}
globalThis.__workerBootstrapTest = {
    runtime: _jsbb_,
    getWorkerParent: getJSWorkerParent,
};`,
        context,
        { filename: `godotjs-web-worker-bootstrap-${label}.js` },
    );
    return context;
}

function createInterop() {
    return {
        gc_callback: () => {},
        unhandled_rejection: () => {},
        call_function: () => {},
        call_accessor: () => {},
        generate_internal_data: () => 1,
        on_worker_message: () => {},
        on_worker_message_from_pthread: () => {},
        worker_get_or_add_native_transfer: () => 0,
        set_web_bridge_profile_enabled: () => {},
        debug_build: false,
        bridge_logging: false,
    };
}

function createPThreadRuntime() {
    const createdWorker = {
        listener: undefined,
        posted: [],
        addEventListener(type, listener) {
            assert.equal(type, "message");
            assert.equal(this.listener, undefined, "the bridge must install only one listener");
            this.listener = listener;
        },
        postMessage(payload) {
            this.posted.push(payload);
        },
    };
    const state = {
        getNewWorkerThis: undefined,
    };
    const originalGetNewWorker = function() {
        state.getNewWorkerThis = this;
        return createdWorker;
    };
    return {
        createdWorker,
        originalGetNewWorker,
        runtime: {
            getNewWorker: originalGetNewWorker,
            pthreads: {
                17: createdWorker,
            },
        },
        state,
    };
}

assertWorkerBootstrapSourceContract(monolithSource);

const lazyHelperSource = `function getPThreadRuntime(): undefined | EmscriptenPThread {
    return typeof PThread !== "undefined" ? PThread : undefined;
}`;
assert.match(monolithSource, new RegExp(lazyHelperSource.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")));
const eagerSnapshotSource = monolithSource.replace(
    lazyHelperSource,
    `const jsbb_pthread_runtime = typeof PThread !== "undefined" ? PThread : undefined;

function getPThreadRuntime(): undefined | EmscriptenPThread {
    return jsbb_pthread_runtime;
}`,
);
assert.notEqual(eagerSnapshotSource, monolithSource, "the eager-snapshot negative control must be constructed");
assert.throws(
    () => assertWorkerBootstrapSourceContract(eagerSnapshotSource),
    /must not retain a module-load PThread snapshot/,
);

const eagerContext = evaluateMonolith(eagerSnapshotSource, "eager-negative-control", false);
const eagerPThread = createPThreadRuntime();
eagerContext.PThread = eagerPThread.runtime;
eagerContext.__workerBootstrapTest.runtime.init(createInterop());
assert.equal(
    eagerPThread.runtime.getNewWorker,
    eagerPThread.originalGetNewWorker,
    "an eager module-load snapshot cannot install the listener after Emscripten declares PThread",
);
assert.equal(
    eagerContext.__workerBootstrapTest.runtime.post_message_to_worker(17, { ready: true }),
    false,
    "an eager module-load snapshot cannot post through a PThread runtime declared later",
);

const mainContext = evaluateMonolith(monolithSource, "lazy-runtime", false);
const firstPThread = createPThreadRuntime();
mainContext.PThread = firstPThread.runtime;
mainContext.__workerBootstrapTest.runtime.init(createInterop());
assert.notEqual(firstPThread.runtime.getNewWorker, firstPThread.originalGetNewWorker);
assert.equal(firstPThread.runtime.getNewWorker.__jsb_wrapped, true);

const wrappedWorker = firstPThread.runtime.getNewWorker();
assert.equal(wrappedWorker, firstPThread.createdWorker);
assert.equal(firstPThread.state.getNewWorkerThis, firstPThread.runtime);
assert.equal(typeof wrappedWorker.listener, "function", "new pthread workers must receive the bridge listener");

let propagationStopped = false;
wrappedWorker.listener({
    data: {
        __jsbMessage: true,
        data: { type: "READY" },
    },
    stopImmediatePropagation: () => {
        propagationStopped = true;
    },
});
assert.equal(propagationStopped, true);
assert.equal(mainContext.__workerBootstrapTest.runtime._pending_worker_messages.length, 1);
assert.equal(mainContext.__workerBootstrapTest.runtime._pending_worker_messages[0].data.type, "READY");

assert.equal(
    mainContext.__workerBootstrapTest.runtime.post_message_to_worker(17, { command: "start" }),
    true,
);
assert.equal(firstPThread.createdWorker.posted.length, 1);
assert.equal(firstPThread.createdWorker.posted[0].__jsbMessage, true);
assert.equal(firstPThread.createdWorker.posted[0].data.command, "start");

const secondPThread = createPThreadRuntime();
mainContext.PThread = secondPThread.runtime;
assert.equal(
    mainContext.__workerBootstrapTest.runtime.post_message_to_worker(17, { command: "reuse" }),
    true,
    "worker posting must use the current PThread runtime",
);
assert.equal(firstPThread.createdWorker.posted.length, 1);
assert.equal(secondPThread.createdWorker.posted.length, 1);
assert.equal(secondPThread.createdWorker.posted[0].data.command, "reuse");

let currentWorkerParent = { name: "first-parent", onmessage: () => {} };
const pthreadContext = evaluateMonolith(
    monolithSource,
    "worker-parent-reset",
    true,
    () => ({ JSWorkerParent: currentWorkerParent }),
);
assert.equal(pthreadContext.__workerBootstrapTest.getWorkerParent().name, "first-parent");
currentWorkerParent = { name: "second-parent", onmessage: () => {} };
assert.equal(
    pthreadContext.__workerBootstrapTest.getWorkerParent().name,
    "first-parent",
    "the logical worker parent is cached within one pthread bridge lifecycle",
);
pthreadContext.__workerBootstrapTest.runtime.reset_pthread_runtime_state();
assert.equal(
    pthreadContext.__workerBootstrapTest.getWorkerParent().name,
    "second-parent",
    "pthread runtime reset must resolve the next logical worker parent",
);

console.log("GodotJS Web worker bootstrap test: PASS");
