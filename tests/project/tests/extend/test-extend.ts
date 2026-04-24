import { ResourceLoader, Script } from "godot";
import Child from "./child";
import { reportTestFailure } from "../test-status";

const ParentOnlyExportName = "parentOnlyExport";

function countScriptProperties(script: Script, propertyName: string): number {
    let count = 0;

    for (const property of script.get_script_property_list()) {
        if (property.get("name") === propertyName) {
            count += 1;
        }
    }

    return count;
}

export default class TestExtend extends Child {
    _ready() {
        try {
            super._ready();
            console.log("TestExtend ready");
            console.assert(this.childFn());

            const parentScript = ResourceLoader.load("res://tests/extend/child.ts");
            const childScript = ResourceLoader.load("res://tests/extend/test-extend.ts");

            if (!(parentScript instanceof Script)) {
                throw new Error("failed to load parent Child script");
            }
            if (!(childScript instanceof Script)) {
                throw new Error("failed to load child TestExtend script");
            }

            const parentExportCount = countScriptProperties(parentScript, ParentOnlyExportName);
            const childExportCount = countScriptProperties(childScript, ParentOnlyExportName);

            if (parentExportCount !== 1) {
                throw new Error(`parent script property count mismatch for ${ParentOnlyExportName}: ${parentExportCount}`);
            }
            if (childExportCount !== 1) {
                throw new Error(`child script inherited property count mismatch for ${ParentOnlyExportName}: ${childExportCount}`);
            }
        } catch (error) {
            reportTestFailure("extend", error);
        }
    }
}
