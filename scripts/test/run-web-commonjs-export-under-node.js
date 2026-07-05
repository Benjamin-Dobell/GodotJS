#!/usr/bin/env node

"use strict";

const {
    existsSync,
    readFileSync,
} = require("node:fs");
const { join, resolve } = require("node:path");

const moduleRoot = resolve(__dirname, "../..");
const defaultExportDir = join(moduleRoot, "tests/bin/web-commonjs-smoke-export");
const defaultSmokeScriptResPath = "res://tests/web-commonjs/web-commonjs-smoke.ts";
const defaultReadySentinel = "GODOTJS_WEB_COMMONJS_READY";
const generatedSmokeScriptPath = ".godot/GodotJS/tests/web-commonjs/web-commonjs-smoke.js";
const generatedNodeConfigPath = "godot.config.cjs";

function getExportDir(p_export_dir) {
    return resolve(p_export_dir || process.env.GODOTJS_WEB_COMMONJS_EXPORT_DIR || defaultExportDir);
}

function ensureFile(path, label) {
    if (!existsSync(path)) {
        throw new Error(`missing ${label}: ${path}`);
    }
}

function validateExportShape(options = {}) {
    const exportDir = getExportDir(options.exportDir);
    const readySentinel = options.readySentinel || defaultReadySentinel;
    const looseSmokeScriptPath = join(exportDir, generatedSmokeScriptPath);
    const nodeConfigPath = join(exportDir, generatedNodeConfigPath);

    ensureFile(join(exportDir, "index.js"), "exported JS glue");
    ensureFile(join(exportDir, "index.wasm"), "exported Wasm");
    ensureFile(join(exportDir, "index.pck"), "exported main pack");
    ensureFile(nodeConfigPath, "post-export Node config");
    ensureFile(looseSmokeScriptPath, "loose generated CommonJS smoke script");

    const nodeConfig = readFileSync(nodeConfigPath, "utf-8");
    if (!nodeConfig.includes("wasmSource") || !nodeConfig.includes("module.exports")) {
        throw new Error(`post-export Node config does not provide wasmSource: ${nodeConfigPath}`);
    }

    const looseScript = readFileSync(looseSmokeScriptPath, "utf-8");
    if (!looseScript.includes("module.exports = function") || !looseScript.includes(readySentinel)) {
        throw new Error(`loose smoke script was not exported as a wrapped CommonJS module: ${looseSmokeScriptPath}`);
    }
    if (existsSync(join(exportDir, "index.side.wasm"))) {
        throw new Error("unexpected side module in no-dlink CommonJS smoke export");
    }
}

function loadExportedWebGlue(exportDir) {
    const indexPath = join(exportDir, "index.js");
    delete require.cache[require.resolve(indexPath)];
    require(indexPath);
    return globalThis.Engine;
}

async function runExportUnderNode(options = {}) {
    const exportDir = getExportDir(options.exportDir);
    const readySentinel = options.readySentinel || defaultReadySentinel;
    const originalLog = console.log;
    const originalError = console.error;
    const capturedLines = [];
    let sawReadySentinel = false;

    function capture(stream, args) {
        const line = args.map((arg) => {
            if (typeof arg === "string") {
                return arg;
            }
            if (arg instanceof Error) {
                return arg.stack || arg.message;
            }
            return String(arg);
        }).join(" ");
        capturedLines.push(line);
        if (line.includes(readySentinel)) {
            sawReadySentinel = true;
        }
        stream.apply(console, args);
    }

    console.log = (...args) => capture(originalLog, args);
    console.error = (...args) => capture(originalError, args);

    let timeout = null;
    try {
        const nodeConfigPath = join(exportDir, generatedNodeConfigPath);
        delete require.cache[require.resolve(nodeConfigPath)];
        const exportedConfig = require(nodeConfigPath);
        if (!exportedConfig || !exportedConfig.wasmSource) {
            throw new Error(`post-export Node config did not load wasmSource: ${nodeConfigPath}`);
        }

        const Engine = loadExportedWebGlue(exportDir);

        if (typeof Engine !== "function") {
            throw new Error("exported Web runtime did not expose globalThis.Engine");
        }

        const packBytes = readFileSync(join(exportDir, "index.pck"));

        const exitPromise = new Promise((resolve, reject) => {
            timeout = setTimeout(() => {
                reject(new Error(`timed out waiting for ${readySentinel}`));
            }, options.timeoutMs || 30000);

            const engine = new Engine({
                ...exportedConfig,
                onPrint: (...args) => capture(originalLog, args),
                onPrintError: (...args) => capture(originalError, args),
                onExit: (code) => resolve(code),
            });

            engine.init()
                .then(() => engine.preloadFile(packBytes, "index.pck"))
                .then(() => engine.start())
                .catch(reject);
        });

        const exitCode = await exitPromise;
        if (!sawReadySentinel) {
            throw new Error(`did not observe ${readySentinel}; tail:\n${capturedLines.slice(-40).join("\n")}`);
        }
        if (exitCode !== 0) {
            throw new Error(`Godot exited with code ${String(exitCode)}`);
        }
    } finally {
        if (timeout) {
            clearTimeout(timeout);
        }
        console.log = originalLog;
        console.error = originalError;
    }
}

async function main() {
    const exportDir = getExportDir(process.argv[2]);
    validateExportShape({ exportDir });
    await runExportUnderNode({ exportDir });
    console.log(`PASS ${defaultReadySentinel} (${defaultSmokeScriptResPath})`);
}

if (require.main === module) {
    main().catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
}

module.exports = {
    defaultExportDir,
    defaultReadySentinel,
    defaultSmokeScriptResPath,
    generatedNodeConfigPath,
    runExportUnderNode,
    validateExportShape,
};
