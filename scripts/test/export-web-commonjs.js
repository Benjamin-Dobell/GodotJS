#!/usr/bin/env node

"use strict";

const { spawnSync } = require("node:child_process");
const {
    existsSync,
    mkdirSync,
    readFileSync,
    rmSync,
    writeFileSync,
} = require("node:fs");
const { join, resolve } = require("node:path");

const moduleRoot = resolve(__dirname, "../..");
const godotRoot = resolve(moduleRoot, "../..");
const projectPath = join(moduleRoot, "tests/project");
const testsBinDir = join(moduleRoot, "tests/bin");
const exportDir = join(testsBinDir, "web-commonjs-smoke-export");
const exportPath = join(exportDir, "index.html");
const nodeConfigPath = join(exportDir, "godot.config.cjs");
const presetPath = join(projectPath, "export_presets.cfg");
const projectConfigPath = join(projectPath, "project.godot");
const templatePath = process.env.GODOTJS_WEB_TEMPLATE
    || join(godotRoot, "bin/godot.web.template_release.wasm32.nothreads.node.zip");
const smokeScene = "res://tests/web-commonjs/WebCommonJSSmoke.tscn";
const pnpmCommand = process.platform === "win32" ? "pnpm.cmd" : "pnpm";

function getEditorBinaryPath() {
    if (process.env.GODOTJS_GODOT_EDITOR) {
        return process.env.GODOTJS_GODOT_EDITOR;
    }

    const arch = process.arch === "arm64" ? "arm64" : "x86_64";
    if (process.platform === "darwin") {
        return join(godotRoot, `bin/godot.macos.editor.${arch}`);
    }
    if (process.platform === "linux") {
        return join(godotRoot, `bin/godot.linuxbsd.editor.${arch}`);
    }
    if (process.platform === "win32") {
        return join(godotRoot, `bin/godot.windows.editor.${arch}.exe`);
    }
    throw new Error(`unsupported host platform: ${process.platform}`);
}

function runCommand(label, command, args, options = {}) {
    const result = spawnSync(command, args, {
        cwd: options.cwd,
        encoding: "utf-8",
        env: options.env,
        stdio: ["ignore", "pipe", "pipe"],
    });
    const output = `${result.stdout || ""}${result.stderr || ""}`;
    process.stdout.write(output);

    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(`${label} failed with exit code ${String(result.status)}`);
    }
    return output;
}

function writeExportPreset() {
    const previous = existsSync(presetPath) ? readFileSync(presetPath, "utf-8") : null;
    const normalizedTemplate = templatePath.replaceAll("\\", "/");
    const normalizedExportPath = exportPath.replaceAll("\\", "/");
    const content = `[preset.0]

name="Web CommonJS Smoke"
platform="Web"
runnable=true
dedicated_server=false
custom_features=""
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path="${normalizedExportPath}"
patches=PackedStringArray()
patch_delta_encoding=false
patch_delta_compression_level_zstd=19
patch_delta_min_reduction=0.1
patch_delta_include_filters="*"
patch_delta_exclude_filters=""
encryption_include_filters=""
encryption_exclude_filters=""
seed=0
encrypt_pck=false
encrypt_directory=false
script_export_mode=2

[preset.0.options]
custom_template/debug="${normalizedTemplate}"
custom_template/release="${normalizedTemplate}"
variant/extensions_support=false
variant/thread_support=false
vram_texture_compression/for_desktop=true
vram_texture_compression/for_mobile=false
html/export_icon=true
html/custom_html_shell=""
html/head_include=""
html/canvas_resize_policy=2
html/focus_canvas_on_start=false
html/experimental_virtual_keyboard=false
progressive_web_app/enabled=false
threads/emscripten_pool_size=0
GodotJS/web_commonjs/enabled=true
`;
    writeFileSync(presetPath, content, "utf-8");

    return function restorePreset() {
        if (previous === null) {
            rmSync(presetPath, { force: true });
        } else {
            writeFileSync(presetPath, previous, "utf-8");
        }
    };
}

function writeProjectMainSceneOverride() {
    const previous = readFileSync(projectConfigPath, "utf-8");
    const currentMainScene = 'run/main_scene="res://tests/Start.tscn"';
    const smokeMainScene = `run/main_scene="${smokeScene}"`;

    if (!previous.includes(currentMainScene)) {
        throw new Error(`project main scene did not match expected test project value in ${projectConfigPath}`);
    }

    writeFileSync(projectConfigPath, previous.replace(currentMainScene, smokeMainScene), "utf-8");

    return function restoreProjectConfig() {
        writeFileSync(projectConfigPath, previous, "utf-8");
    };
}

function ensureFile(path, label) {
    if (!existsSync(path)) {
        throw new Error(`missing ${label}: ${path}`);
    }
}

function readExportedGodotConfig() {
    ensureFile(exportPath, "exported HTML");

    const html = readFileSync(exportPath, "utf-8");
    const match = html.match(/\bconst GODOT_CONFIG = (\{[^\n]*\});/);
    if (!match) {
        throw new Error(`could not find GODOT_CONFIG in ${exportPath}`);
    }

    try {
        return JSON.parse(match[1]);
    } catch (error) {
        throw new Error(`could not parse GODOT_CONFIG from ${exportPath}: ${error.message}`);
    }
}

function writeNodeConfigPostExport() {
    const config = {
        ...readExportedGodotConfig(),
        locale: "en",
        persistentPaths: [],
        args: ["--headless", "--main-pack", "index.pck"],
    };

    const content = `// Generated by scripts/test/export-web-commonjs.js for the Node CommonJS smoke runner.
"use strict";

const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const config = ${JSON.stringify(config, null, 4)};
config.wasmSource = readFileSync(join(__dirname, \`\${config.executable}.wasm\`));
config.godotJSHostRequire = require;

module.exports = config;
`;
    writeFileSync(nodeConfigPath, content, "utf-8");
}

function buildProjectScripts(editorPath) {
    runCommand("test project TypeScript build", pnpmCommand, ["build"], {
        cwd: projectPath,
        env: {
            ...process.env,
            GODOT: process.env.GODOT || editorPath,
        },
    });
}

function exportWebCommonJS() {
    const editorPath = getEditorBinaryPath();
    ensureFile(editorPath, "Godot editor binary");
    ensureFile(templatePath, "Web release no-thread template");
    buildProjectScripts(editorPath);

    rmSync(exportDir, { recursive: true, force: true });
    mkdirSync(exportDir, { recursive: true });

    const restorePreset = writeExportPreset();
    let restoreProjectConfig = () => {};
    try {
        restoreProjectConfig = writeProjectMainSceneOverride();
        runCommand("web commonjs export", editorPath, [
            "--headless",
            "--path",
            projectPath,
            "--export-release",
            "Web CommonJS Smoke",
            exportPath,
        ], { cwd: projectPath });
    } finally {
        restoreProjectConfig();
        restorePreset();
    }

    writeNodeConfigPostExport();

    return exportDir;
}

function main() {
    const exportedDir = exportWebCommonJS();
    console.log(`EXPORT ${exportedDir}`);
}

if (require.main === module) {
    try {
        main();
    } catch (error) {
        console.error(error);
        process.exitCode = 1;
    }
}

module.exports = {
    exportDir,
    exportWebCommonJS,
    nodeConfigPath,
};
