import { FileAccess, Node } from "godot";
import { beginAsyncTest, endAsyncTest, reportTestFailure } from "../test-status";

declare function require(id: string): any;

const jsb = require("godot-jsb") as {
    internal: {
        replace_module_source: (id: string, source: string) => string[];
    };
};

const BASE_PATH = "res://tests/reload/runtime-replace-base.js";
const DEPENDENT_PATH = "res://tests/reload/runtime-replace-dependent.js";
const VIRTUAL_PATH = "res://tests/reload/runtime-replace-virtual.js";
const SCRIPT_TARGET_MODULE = "res://.godot/GodotJS/tests/reload/runtime-replace-script-target.js";
const POLICY_TARGET_MODULE = "res://.godot/GodotJS/tests/reload/runtime-replace-policy-target.js";

function expectDirtyModules(actual: string[], expected: string[]): void {
    for (const id of expected) {
        if (!actual.includes(id)) {
            throw new Error(`expected dirty module ${id}; got [${actual.join(", ")}]`);
        }
    }
}

function readFile(path: string): string {
    const reader = FileAccess.open(path, FileAccess.ModeFlags.READ);
    if (!reader) throw new Error(`failed to open ${path} for read`);
    const source = reader.get_as_text();
    reader.close();
    return source;
}

function writeFile(path: string, content: string): void {
    const writer = FileAccess.open(path, FileAccess.ModeFlags.WRITE);
    if (!writer) throw new Error(`failed to open ${path} for write`);
    writer.store_string(content);
    writer.close();
}

async function runRuntimeReplaceTest(owner: Node): Promise<void> {
    const scriptTargetBefore = owner.get_node("ScriptTarget") as any;
    if (scriptTargetBefore.getValue() !== 1) {
        throw new Error(`script target baseline returned ${scriptTargetBefore.getValue()} (expected 1)`);
    }

    const dirtyFromScript = jsb.internal.replace_module_source(
        SCRIPT_TARGET_MODULE,
        [
            'const godot = require("godot");',
            "class RuntimeReplaceScriptTarget extends godot.Node {",
            "    #value = 2;",
            "    getValue() { return this.#value; }",
            "}",
            "module.exports.default = RuntimeReplaceScriptTarget;",
            "",
        ].join("\n"),
    );
    expectDirtyModules(dirtyFromScript, [SCRIPT_TARGET_MODULE]);

    const scriptTargetAfter = owner.get_node("ScriptTarget") as any;
    const scriptValueAfter = scriptTargetAfter.getValue();
    if (scriptValueAfter !== 2) {
        throw new Error(`script target after replacement returned ${scriptValueAfter} (expected 2)`);
    }

    const policyTargetBefore = owner.get_node("PolicyTarget") as any;
    if (policyTargetBefore.getValue() !== 3) {
        throw new Error(`policy target baseline returned ${policyTargetBefore.getValue()} (expected 3)`);
    }

    const dirtyFromPolicyScript = jsb.internal.replace_module_source(
        POLICY_TARGET_MODULE,
        [
            'const godot = require("godot");',
            "class RuntimeReplacePolicyTarget extends godot.Node {",
            "    #value = 4;",
            "    getValue() { return this.#value; }",
            "}",
            "module.exports.default = RuntimeReplacePolicyTarget;",
            "",
        ].join("\n"),
    );
    expectDirtyModules(dirtyFromPolicyScript, [POLICY_TARGET_MODULE]);

    const policyTargetAfter = owner.get_node("PolicyTarget") as any;
    const policyValueAfter = policyTargetAfter.getValue();
    if (policyValueAfter !== 4) {
        throw new Error(`policy target after replacement returned ${policyValueAfter} (expected 4)`);
    }

    let invalidTargetThrew = false;
    try {
        jsb.internal.replace_module_source("godot", "module.exports = {};\n");
    } catch {
        invalidTargetThrew = true;
    }
    if (!invalidTargetThrew) {
        throw new Error("replacing builtin godot module did not throw");
    }

    const dirtyFromVirtualInitial = jsb.internal.replace_module_source(
        VIRTUAL_PATH,
        "module.exports.value = 7;\n",
    );
    if (dirtyFromVirtualInitial.length !== 0) {
        throw new Error(`unloaded virtual replacement should not dirty loaded modules; got [${dirtyFromVirtualInitial.join(", ")}]`);
    }

    const virtualInitial = require(VIRTUAL_PATH);
    if (virtualInitial.value !== 7) {
        throw new Error(`virtual initial: value returned ${virtualInitial.value} (expected 7)`);
    }

    const dirtyFromVirtualLoaded = jsb.internal.replace_module_source(
        VIRTUAL_PATH,
        "module.exports.value = 8;\n",
    );
    expectDirtyModules(dirtyFromVirtualLoaded, [VIRTUAL_PATH]);

    const virtualReloaded = require(VIRTUAL_PATH);
    if (virtualReloaded.value !== 8) {
        throw new Error(`virtual reload: value returned ${virtualReloaded.value} (expected 8)`);
    }

    let badReplacementThrew = false;
    try {
        jsb.internal.replace_module_source(VIRTUAL_PATH, "module.exports = ;\n");
    } catch {
        badReplacementThrew = true;
    }
    if (!badReplacementThrew) {
        throw new Error("bad virtual replacement source did not throw");
    }

    const virtualAfterBadReplacement = require(VIRTUAL_PATH);
    if (virtualAfterBadReplacement.value !== 8) {
        throw new Error(`virtual after bad replacement returned ${virtualAfterBadReplacement.value} (expected previous value 8)`);
    }

    const dirtyFromVirtualAfterBad = jsb.internal.replace_module_source(
        VIRTUAL_PATH,
        "module.exports.value = 9;\n",
    );
    expectDirtyModules(dirtyFromVirtualAfterBad, [VIRTUAL_PATH]);

    const virtualAfterRollback = require(VIRTUAL_PATH);
    if (virtualAfterRollback.value !== 9) {
        throw new Error(`virtual after rollback returned ${virtualAfterRollback.value} (expected 9)`);
    }

    const beforeDependent = require(DEPENDENT_PATH);
    const beforeCombined = beforeDependent.getCombined();
    if (beforeCombined !== 11) {
        throw new Error(`baseline: getCombined() returned ${beforeCombined} (expected 11)`);
    }

    const dirtyFromBase = jsb.internal.replace_module_source(
        BASE_PATH,
        "module.exports.baseValue = function baseValue() { return 100; };\n",
    );
    expectDirtyModules(dirtyFromBase, [BASE_PATH, DEPENDENT_PATH]);

    const afterBase = require(BASE_PATH);
    const afterBaseValue = afterBase.baseValue();
    if (afterBaseValue !== 100) {
        throw new Error(`post-base-replace: baseValue() returned ${afterBaseValue} (expected 100)`);
    }

    const afterDependent = require(DEPENDENT_PATH);
    const afterCombined = afterDependent.getCombined();
    if (afterCombined !== 101) {
        throw new Error(`post-base-replace: getCombined() returned ${afterCombined} (expected 101)`);
    }

    const dependentOriginal = readFile(DEPENDENT_PATH);
    let transitiveFailureThrew = false;
    try {
        writeFile(DEPENDENT_PATH, "module.exports = ;\n");
        try {
            jsb.internal.replace_module_source(
                BASE_PATH,
                "module.exports.baseValue = function baseValue() { return 200; };\n",
            );
        } catch {
            transitiveFailureThrew = true;
        }
    } finally {
        writeFile(DEPENDENT_PATH, dependentOriginal);
    }
    if (!transitiveFailureThrew) {
        throw new Error("bad transitive dependent replacement did not throw");
    }

    const baseAfterFailedTransitive = require(BASE_PATH);
    const baseValueAfterFailedTransitive = baseAfterFailedTransitive.baseValue();
    if (baseValueAfterFailedTransitive !== 100) {
        throw new Error(`transitive failure rollback left baseValue() at ${baseValueAfterFailedTransitive} (expected 100)`);
    }

    const dependentAfterFailedTransitive = require(DEPENDENT_PATH);
    const combinedAfterFailedTransitive = dependentAfterFailedTransitive.getCombined();
    if (combinedAfterFailedTransitive !== 101) {
        throw new Error(`transitive failure rollback left getCombined() at ${combinedAfterFailedTransitive} (expected 101)`);
    }

    const dirtyFromDependent = jsb.internal.replace_module_source(
        DEPENDENT_PATH,
        [
            'const base = require("./runtime-replace-base");',
            "module.exports.getCombined = function getCombined() { return base.baseValue() + 2; };",
            "",
        ].join("\n"),
    );
    expectDirtyModules(dirtyFromDependent, [DEPENDENT_PATH]);

    const replacedDependent = require(DEPENDENT_PATH);
    const replacedCombined = replacedDependent.getCombined();
    if (replacedCombined !== 102) {
        throw new Error(`post-dependent-replace: getCombined() returned ${replacedCombined} (expected 102)`);
    }

    console.log("TestRuntimeReplace: runtime module source replacement OK");
}

export default class TestRuntimeReplace extends Node {
    _ready(): void {
        beginAsyncTest();
        runRuntimeReplaceTest(this)
            .catch((error) => reportTestFailure("runtime-replace", error))
            .finally(() => endAsyncTest());
    }
}
