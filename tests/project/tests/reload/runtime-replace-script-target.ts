import { Node } from "godot";

export default class RuntimeReplaceScriptTarget extends Node {
    #value = 1;

    getValue(): number {
        return this.#value;
    }
}
