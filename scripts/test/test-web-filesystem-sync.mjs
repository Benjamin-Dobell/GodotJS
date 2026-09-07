import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const godotRoot = resolve(moduleRoot, "../..");
const godotFilesystemLibraryPath = join(godotRoot, "platform/web/js/libs/library_godot_os.js");
const godotJSFilesystemLibraryPath = join(moduleRoot, "impl/web/js/library_godotjs_fs.js");

const godotFilesystemLibrarySource = readFileSync(godotFilesystemLibraryPath, "utf8")
    .replace("const GodotFS = {", "const GodotFSLibrary = {")
    .replace(
        "mergeInto(LibraryManager.library, GodotFS);",
        "mergeInto(LibraryManager.library, GodotFSLibrary);",
    );
const godotJSFilesystemLibrarySource = readFileSync(godotJSFilesystemLibraryPath, "utf8");

const nativeSyncs = [];
const runtimeErrors = [];
let activeNativeSyncs = 0;
let maximumActiveNativeSyncs = 0;

const filesystem = {
    syncfs(populate, callback) {
        assert.equal(populate, false, "persistence writes must not populate MEMFS from IndexedDB");
        ++activeNativeSyncs;
        maximumActiveNativeSyncs = Math.max(maximumActiveNativeSyncs, activeNativeSyncs);
        nativeSyncs.push((error = null) => {
            --activeNativeSyncs;
            callback(error);
        });
    },
};

const context = vm.createContext({
    Error,
    FS: filesystem,
    IDBFS: { dbs: {} },
    LibraryManager: { library: {} },
    Module: {},
    Promise,
    Uint8Array,
    autoAddDeps() {},
    GodotRuntime: {
        error(...args) {
            runtimeErrors.push(args.map(String).join(" "));
        },
        print() {},
    },
    mergeInto(target, source) {
        Object.assign(target, source);
    },
});

vm.runInContext(godotFilesystemLibrarySource, context, {
    filename: godotFilesystemLibraryPath,
});
vm.runInContext(godotJSFilesystemLibrarySource, context, {
    filename: godotJSFilesystemLibraryPath,
});

const godotFilesystem = context.LibraryManager.library.$GodotFS;
const godotJSFilesystem = context.LibraryManager.library.$GodotJSFileSystem;
assert.ok(godotFilesystem, "Godot Web filesystem owner must be registered");
assert.ok(godotJSFilesystem, "GodotJS filesystem bridge must be registered");
context.GodotFS = godotFilesystem;
assert.deepEqual(
    Array.from(context.LibraryManager.library.$GodotJSFileSystem__deps),
    ["$GodotFS"],
    "the GodotJS bridge must depend on the common Godot Web synchronization owner",
);

const automaticSync = godotFilesystem.sync();
let explicitSyncSettled = false;
const explicitSync = new Promise((resolveSync) => {
    assert.equal(
        godotJSFilesystem.sync((error) => {
            explicitSyncSettled = true;
            resolveSync(error);
        }),
        true,
        "the bridge must accept a callable completion callback",
    );
});

await Promise.resolve();
assert.equal(maximumActiveNativeSyncs, 1, "automatic and explicit persistence writes must never overlap");
assert.equal(nativeSyncs.length, 1, "the explicit persistence barrier must queue behind the active automatic write");
assert.equal(explicitSyncSettled, false, "the explicit barrier must not report an earlier automatic write as its own");

nativeSyncs.shift()(null);
await automaticSync;
await Promise.resolve();
assert.equal(nativeSyncs.length, 1, "the explicit barrier must start its own pass after the automatic write");
assert.equal(explicitSyncSettled, false, "the explicit callback must wait for its queued native pass");

nativeSyncs.shift()(null);
assert.equal(await explicitSync, null, "a successful queued explicit pass must report no error");
assert.equal(activeNativeSyncs, 0, "all native persistence writes must finish");
assert.equal(maximumActiveNativeSyncs, 1, "the common owner must keep native concurrency at one");

const transientMissingPathError = Object.assign(new Error("No such file or directory"), {
    errno: godotFilesystem.ENOENT,
    path: "/userfs/runtime-snapshots/transient.sqlite",
});
let transientSyncSettled = false;
const transientSync = new Promise((resolveSync) => {
    assert.equal(
        godotJSFilesystem.sync((error) => {
            transientSyncSettled = true;
            resolveSync(error);
        }),
        true,
    );
});
await Promise.resolve();
assert.equal(nativeSyncs.length, 1, "a transient-race test must start one native pass");
nativeSyncs.shift()(transientMissingPathError);
await Promise.resolve();
await Promise.resolve();
assert.equal(nativeSyncs.length, 1, "ENOENT must trigger a fresh reconciliation pass");
assert.equal(transientSyncSettled, false, "the bridge must wait for the retry");
assert.equal(runtimeErrors.length, 0, "a recoverable stale-path race must not emit a persistence error");
nativeSyncs.shift()(null);
assert.equal(await transientSync, null, "a successful ENOENT retry must satisfy the explicit barrier");
assert.equal(maximumActiveNativeSyncs, 1, "ENOENT recovery must remain single-flight");

const expectedError = new Error("No such file or directory");
const failedExplicitSync = new Promise((resolveSync) => {
    assert.equal(godotJSFilesystem.sync(resolveSync), true);
});
await Promise.resolve();
assert.equal(nativeSyncs.length, 1, "a later explicit barrier must start one native pass");
nativeSyncs.shift()(expectedError);
assert.equal(await failedExplicitSync, expectedError, "the bridge must preserve the native synchronization error");
assert.ok(
    runtimeErrors.some((message) => message.includes("Failed to save IDB file system: No such file or directory")),
    "the common Godot owner must retain its existing persistence-error diagnostic",
);

const exhaustedMissingPathError = Object.assign(new Error("No such file or directory"), {
    errno: godotFilesystem.ENOENT,
    path: "/userfs/runtime-snapshots/continuously-changing.sqlite",
});
const exhaustedSync = new Promise((resolveSync) => {
    assert.equal(godotJSFilesystem.sync(resolveSync), true);
});
await Promise.resolve();
for (let attempt = 1; attempt <= 3; ++attempt) {
    assert.equal(nativeSyncs.length, 1, `ENOENT attempt ${attempt} must have exactly one native pass`);
    nativeSyncs.shift()(exhaustedMissingPathError);
    await Promise.resolve();
    await Promise.resolve();
}
assert.equal(
    await exhaustedSync,
    exhaustedMissingPathError,
    "a continuously changing filesystem must surface ENOENT after bounded retries",
);
assert.equal(
    runtimeErrors.filter((message) => message.includes("Failed to save IDB file system: No such file or directory")).length,
    2,
    "only the hard failure and exhausted transient failure must emit persistence diagnostics",
);
assert.equal(maximumActiveNativeSyncs, 1, "retry exhaustion must not introduce native overlap");

assert.equal(godotJSFilesystem.sync(null), false, "the bridge must reject a non-callable callback");
assert.equal(nativeSyncs.length, 0, "an invalid bridge call must not enqueue native work");

console.log("Web filesystem synchronization queue checks passed.");
