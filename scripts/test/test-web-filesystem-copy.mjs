import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const testDirectory = dirname(fileURLToPath(import.meta.url));
const moduleRoot = resolve(testDirectory, "../..");
const godotRoot = resolve(moduleRoot, "../..");
const enginePath = join(godotRoot, "platform/web/js/engine/engine.js");
const featuresPath = join(godotRoot, "platform/web/js/engine/features.js");
const preloaderPath = join(godotRoot, "platform/web/js/engine/preloader.js");
const godotFilesystemLibraryPath = join(godotRoot, "platform/web/js/libs/library_godot_os.js");

const engineSource = readFileSync(enginePath, "utf8");
const featuresSource = readFileSync(featuresPath, "utf8");
const preloaderSource = readFileSync(preloaderPath, "utf8");
const godotFilesystemLibrarySource = readFileSync(godotFilesystemLibraryPath, "utf8")
    .replace("const GodotFS = {", "const GodotFSLibrary = {")
    .replace(
        "mergeInto(LibraryManager.library, GodotFS);",
        "mergeInto(LibraryManager.library, GodotFSLibrary);",
    );

assert.match(
    engineSource,
    /me\.rtenv\['copyToFS'\]\(file\.path, file\.buffer, file\.canOwn === true\);/,
    "Engine.start must opt only preloader-owned buffers into the private ownership handoff",
);
assert.match(
    engineSource,
    /this\.rtenv\['copyToFS'\]\(path, buffer\);/,
    "public Engine.copyToFS must retain its non-owning call shape",
);

const files = new Map();
const directories = new Set(["/"]);
const writeOptions = [];
const runtimeErrors = [];

const filesystem = {
    stat(path) {
        if (!directories.has(path)) {
            throw Object.assign(new Error(`missing directory: ${path}`), { errno: 44 });
        }
        return { mode: 0 };
    },
    mkdirTree(path) {
        directories.add(path);
    },
    writeFile(path, data, options) {
        assert.ok(data instanceof Uint8Array, "GodotFS must pass byte data to MEMFS");
        const normalizedOptions = options ?? {};
        writeOptions.push({ path, options: normalizedOptions, hasOptions: options !== undefined });
        if (path === "/retry.bin" && !files.has("/retry-attempted")) {
            files.set("/retry-attempted", new Uint8Array(0));
            throw new Error("transient write failure");
        }
        // Model MEMFS's first-write behavior: adoption aliases the incoming view;
        // the default path makes an independent copy.
        files.set(path, normalizedOptions.canOwn === true ? data.subarray(0) : data.slice());
    },
};

const context = vm.createContext({
    ArrayBuffer,
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

const godotFilesystem = context.LibraryManager.library.$GodotFS;
assert.ok(godotFilesystem, "Godot Web filesystem owner must be registered");
context.GodotFS = godotFilesystem;

const publicBacking = new Uint8Array([10, 11, 12, 13, 14]);
const publicView = publicBacking.subarray(1, 4);
godotFilesystem.copy_to_fs("/public/view.bin", publicView);
publicBacking[2] = 99;
assert.deepEqual(
    Array.from(files.get("/public/view.bin")),
    [11, 12, 13],
    "public typed-array views must be copied and preserve offset/length",
);
assert.equal(
    writeOptions.at(-1).hasOptions,
    false,
    "public copyToFS calls must retain the original two-argument FS.writeFile shape",
);

const publicBufferBacking = new Uint8Array([20, 21, 22]);
godotFilesystem.copy_to_fs("/public/array-buffer.bin", publicBufferBacking.buffer);
publicBufferBacking[0] = 77;
assert.deepEqual(
    Array.from(files.get("/public/array-buffer.bin")),
    [20, 21, 22],
    "public ArrayBuffer input must remain mutation-independent",
);

const ownedBacking = new Uint8Array([30, 31, 32, 33, 34]);
const ownedView = ownedBacking.subarray(1, 4);
godotFilesystem.copy_to_fs("/internal/owned.bin", ownedView, true);
const adopted = files.get("/internal/owned.bin");
assert.equal(adopted.buffer, ownedView.buffer, "private handoff must adopt the caller-owned backing store");
assert.equal(adopted.byteOffset, ownedView.byteOffset, "private handoff must preserve a subview byte offset");
assert.equal(adopted.byteLength, ownedView.byteLength, "private handoff must preserve a subview byte length");
ownedBacking[2] = 88;
assert.deepEqual(
    Array.from(adopted),
    [31, 88, 33],
    "private ownership adoption must retain exact subview aliasing",
);
assert.equal(
    writeOptions.at(-1).options.canOwn,
    true,
    "only the private preloader handoff may opt into ownership adoption",
);

godotFilesystem.copy_to_fs("/internal/empty.bin", new Uint8Array(0), true);
assert.equal(files.get("/internal/empty.bin").byteLength, 0, "zero-length owned buffers must remain valid");

assert.throws(
    () => godotFilesystem.copy_to_fs("/retry.bin", new Uint8Array([40, 41]), true),
    /transient write failure/,
    "a failed private write must surface its write error",
);
godotFilesystem.copy_to_fs("/retry.bin", new Uint8Array([40, 41]), true);
assert.deepEqual(
    Array.from(files.get("/retry.bin")),
    [40, 41],
    "a retried private write must mount exact bytes after a transient failure",
);
assert.deepEqual(runtimeErrors, [], "copy handoff must not emit unrelated runtime diagnostics");

// Exercise the real Preloader + Engine.start path. String loads are fetched and
// marked adoptable; caller-provided buffers are staged as non-owning records.
const integrationFiles = new Map();
const integrationCopies = [];
const integrationDirectories = new Set(["/"]);
const integrationFilesystem = {
    stat(path) {
        if (!integrationDirectories.has(path)) {
            throw Object.assign(new Error(`missing directory: ${path}`), { errno: 44 });
        }
        return { mode: 0 };
    },
    mkdirTree(path) {
        integrationDirectories.add(path);
    },
    writeFile(path, data, options) {
        assert.ok(data instanceof Uint8Array, "Engine.start must hand byte data to MEMFS");
        const canOwn = options?.canOwn === true;
        integrationCopies.push({ path, data, canOwn });
        integrationFiles.set(path, canOwn ? data.subarray(0) : data.slice());
    },
};

const integrationContext = vm.createContext({
    ArrayBuffer,
    Error,
    FS: integrationFilesystem,
    IDBFS: { dbs: {} },
    LibraryManager: { library: {} },
    Module: {},
    Promise,
    ReadableStream,
    Response,
    Uint8Array,
    autoAddDeps() {},
    clearTimeout,
    fetch(url) {
        if (url === "retry.bin" && integrationFetchAttempts++ === 0) {
            return Promise.resolve(new Response(null, { status: 503 }));
        }
        const payload = url === "fetched.bin" ? [50, 51, 52] : [60, 61, 62];
        return Promise.resolve(new Response(new Uint8Array(payload)));
    },
    Godot: null,
    GodotRuntime: {
        error() {},
        print() {},
    },
    mergeInto(target, source) {
        Object.assign(target, source);
    },
    setTimeout(callback) {
        Promise.resolve().then(callback);
        return 1;
    },
});
let integrationFetchAttempts = 0;

vm.runInContext(godotFilesystemLibrarySource, integrationContext, {
    filename: godotFilesystemLibraryPath,
});
integrationContext.GodotFS = integrationContext.LibraryManager.library.$GodotFS;
vm.runInContext(preloaderSource, integrationContext, { filename: preloaderPath });
vm.runInContext(featuresSource, integrationContext, { filename: featuresPath });
integrationContext.InternalConfig = function InternalConfig(initConfig = {}) {
    this.executable = "index";
    this.fileSizes = {};
    this.gdextensionLibs = [];
    this.args = [];
    this.persistentPaths = [];
    this.unloadAfterInit = false;
    this.wasmSource = Promise.resolve(new ArrayBuffer(0));
    Object.assign(this, initConfig);
};
integrationContext.InternalConfig.prototype.update = function update(override) {
    if (override) {
        Object.assign(this, override);
    }
};
integrationContext.InternalConfig.prototype.getModuleConfig = function getModuleConfig() {
    return {};
};
integrationContext.InternalConfig.prototype.getGodotConfig = function getGodotConfig() {
    return {};
};
integrationContext.Godot = function Godot() {
    return Promise.resolve({
        initFS: () => Promise.resolve(),
        initConfig() {},
        copyToFS(path, data, canOwn) {
            integrationContext.GodotFS.copy_to_fs(path, data, canOwn);
        },
        callMain() {},
    });
};
vm.runInContext(engineSource, integrationContext, { filename: enginePath });

const integrationEngine = new integrationContext.Engine({
    executable: "index",
    wasmSource: Promise.resolve(new ArrayBuffer(0)),
    unloadAfterInit: false,
});
await integrationEngine.preloadFile("fetched.bin", "fetched.bin");
await integrationEngine.preloadFile("retry.bin", "retry.bin");
const callerBacking = new Uint8Array([70, 71, 72, 73]);
const callerView = callerBacking.subarray(1, 3);
await integrationEngine.preloadFile(callerView, "caller.bin");
await integrationEngine.start();

assert.equal(integrationFetchAttempts, 2, "fetched preload records must preserve bounded retry behavior");
assert.deepEqual(integrationCopies.map(({ path }) => path), ["fetched.bin", "retry.bin", "caller.bin"]);
assert.deepEqual(integrationCopies.map(({ canOwn }) => canOwn), [true, true, false]);
assert.deepEqual(Array.from(integrationFiles.get("fetched.bin")), [50, 51, 52]);
assert.deepEqual(Array.from(integrationFiles.get("retry.bin")), [60, 61, 62]);
assert.deepEqual(Array.from(integrationFiles.get("caller.bin")), [71, 72]);
callerBacking[1] = 99;
assert.deepEqual(
    Array.from(integrationFiles.get("caller.bin")),
    [71, 72],
    "caller preloadFile input must remain mutation-independent after Engine.start",
);
assert.equal(
    integrationFiles.get("fetched.bin").buffer,
    integrationCopies[0].data.buffer,
    "internally fetched preload must adopt the MEMFS backing store",
);

console.log("Web filesystem ownership handoff checks passed.");
