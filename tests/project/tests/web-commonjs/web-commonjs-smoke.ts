import { FileAccess, Node } from "godot";

const READY_SENTINEL = "GODOTJS_WEB_COMMONJS_READY";

export default class WebCommonJSSmoke extends Node {
    _ready() {
        const staticAsset = FileAccess.open("res://static/static-test.json", FileAccess.ModeFlags.READ);
        if (!staticAsset) {
            throw new Error("static asset was not packaged");
        }

        const content = staticAsset.get_as_text();
        if (!content.includes("\"source\": \"static-assets\"")) {
            throw new Error("static asset content did not match");
        }

        const gdignore = FileAccess.open("res://static/.gdignore", FileAccess.ModeFlags.READ);
        if (gdignore) {
            throw new Error(".gdignore marker should not be packaged as a static asset");
        }

        console.log(READY_SENTINEL);
        this.get_tree().quit();
    }
}
