import { Node } from "godot";
import { createClassBinder, HotReload } from "godot.annotations";

const bind = createClassBinder();

@bind.hotReload(HotReload.Prototype)
@bind()
export default class RuntimeReplacePolicyTarget extends Node {
    #value = 3;

    getValue(): number {
        return this.#value;
    }
}
