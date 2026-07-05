import { Node } from "godot";

const READY_SENTINEL = "GODOTJS_WEB_COMMONJS_READY";

export default class WebCommonJSSmoke extends Node {
    _ready() {
        console.log(READY_SENTINEL);
        this.get_tree().quit();
    }
}
