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
const refPath = join(moduleRoot, "bridge/jsb_ref.h");
const webDataHeaderPath = join(moduleRoot, "impl/web/jsb_web_data.h");

const monolithSource = readFileSync(monolithPath, "utf8");
const librarySource = readFileSync(libraryPath, "utf8");
const refSource = readFileSync(refPath, "utf8");
const webDataHeaderSource = readFileSync(webDataHeaderPath, "utf8");
const strictReferenceFallbacks = refSource.match(
    /return\s*\/\/ lhs\.hash_ == rhs\.hash_ &&\s*lhs\.object_ == rhs\.object_;/g,
) ?? [];
assert.equal(strictReferenceFallbacks.length, 2, "weak and strong reference equality must ignore hash collisions");
assert.match(
    webDataHeaderSource,
    /stack_pos_ == other\.stack_pos_ \|\| strict_eq\(other\)/,
    "Web data equality must retain strict JavaScript identity fallback",
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
let nextAllocation = 1024;
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
    Float64Array,
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
globalThis.__identityHashTest = { runtime: _jsbb_, opaque: jsbb_opaque };
${librarySource}`,
    context,
    { filename: "godotjs-web-identity-hash-test.js" },
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

const runtime = context.__identityHashTest.runtime;
const engine = runtime.GetEngine(0);
const push = (value) => engine.stack.Push(value);
const hash = (value) => bridge.jsbi_hash(0, push(value));

assert.equal(hash(undefined), 0);
assert.equal(hash(null), 0);
assert.equal(hash(true), 1);
assert.equal(hash(false), 0);
assert.equal(hash(1), 1);
assert.equal(hash(0), 48);
assert.equal(hash(""), 0);
assert.equal(hash("hash"), 3195150);
assert.equal(hash(0n), 48);

const object = {};
const objectHash = hash(object);
assert.notEqual(objectHash, 0, "ordinary objects must receive a non-zero identity hash");
assert.equal(hash(object), objectHash, "an object hash must remain stable");
assert.notEqual(hash({}), objectHash, "distinct objects should receive distinct hashes before wraparound");

const func = function identityHashFunction() {};
const functionHash = hash(func);
assert.notEqual(functionHash, 0, "ordinary functions must receive a non-zero identity hash");
assert.equal(hash(func), functionHash, "a function hash must remain stable");
assert.notEqual(hash(() => {}), functionHash, "distinct functions should receive distinct hashes before wraparound");

const frozen = Object.freeze({ frozen: true });
const frozenKeys = Reflect.ownKeys(frozen);
assert.notEqual(hash(frozen), 0, "frozen objects must be hashable");
assert.deepEqual(Reflect.ownKeys(frozen), frozenKeys, "hashing must not mutate frozen objects");

const nonExtensible = Object.preventExtensions({ nonExtensible: true });
const nonExtensibleKeys = Reflect.ownKeys(nonExtensible);
assert.notEqual(hash(nonExtensible), 0, "non-extensible objects must be hashable");
assert.deepEqual(Reflect.ownKeys(nonExtensible), nonExtensibleKeys, "hashing must not mutate non-extensible objects");

const crossRealmObject = vm.runInNewContext("({ crossRealm: true })");
const crossRealmFunction = vm.runInNewContext("(function crossRealmFunction() {})");
assert.notEqual(hash(crossRealmObject), 0, "cross-realm objects must be hashable");
assert.notEqual(hash(crossRealmFunction), 0, "cross-realm functions must be hashable");

const identityHashes = engine._identity_hashes;
assert.ok(identityHashes instanceof WeakMap, "ordinary identities must be held weakly");

const boundObject = {};
engine._registry.Add(boundObject, 4242);
assert.equal(hash(boundObject), 4242, "bound objects must retain their opaque ID hash");
assert.equal(identityHashes.has(boundObject), false, "bound objects must not enter the ordinary identity table");

const boundFunction = function boundFunction() {};
engine._registry.Add(boundFunction, 5252);
assert.equal(hash(boundFunction), 5252, "bound functions must retain their opaque ID hash");
assert.equal(identityHashes.has(boundFunction), false, "bound functions must not enter the ordinary identity table");

const localSymbol = vm.runInContext("Symbol('local')", context);
const otherLocalSymbol = vm.runInContext("Symbol('local')", context);
const globalSymbol = vm.runInContext("Symbol.for('godotjs.identity-hash-test')", context);
const localSymbolPosition = push(localSymbol);
const duplicateLocalSymbolPosition = push(localSymbol);
const otherLocalSymbolPosition = push(otherLocalSymbol);
assert.equal(bridge.jsbi_hash(0, localSymbolPosition), 0, "local symbols intentionally use strict-equality fallback");
assert.equal(bridge.jsbi_hash(0, push(globalSymbol)), 0, "global symbols intentionally use strict-equality fallback");
assert.equal(bridge.jsbi_stack_eq(0, localSymbolPosition, duplicateLocalSymbolPosition), true);
assert.equal(bridge.jsbi_stack_eq(0, localSymbolPosition, otherLocalSymbolPosition), false);

const collisionA = {};
const collisionB = {};
identityHashes.set(collisionA, 7171);
identityHashes.set(collisionB, 7171);
const collisionAPosition = push(collisionA);
const duplicateCollisionAPosition = push(collisionA);
const collisionBPosition = push(collisionB);
assert.equal(bridge.jsbi_hash(0, collisionAPosition), bridge.jsbi_hash(0, collisionBPosition));
assert.equal(bridge.jsbi_stack_eq(0, collisionAPosition, duplicateCollisionAPosition), true);
assert.equal(
    bridge.jsbi_stack_eq(0, collisionAPosition, collisionBPosition),
    false,
    "strict equality must resolve hash collisions",
);

const oldEngine = engine;
const oldIdentityHashes = identityHashes;
bridge.jsbi_FreeEngine(0);
assert.equal(oldEngine._identity_hashes, undefined, "engine release must drop the weak identity table");

assert.equal(bridge.jsbi_NewEngine(5678), 0);
const newEngine = runtime.GetEngine(0);
assert.ok(newEngine._identity_hashes instanceof WeakMap);
assert.notEqual(newEngine._identity_hashes, oldIdentityHashes, "new engines must own independent identity state");
assert.notEqual(bridge.jsbi_hash(0, newEngine.stack.Push({})), 0);
bridge.jsbi_FreeEngine(0);

console.log("GodotJS Web identity hash test: PASS");
