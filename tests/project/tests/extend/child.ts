import { Node, Variant } from "godot";
import { createClassBinder } from "godot.annotations";

const bind = createClassBinder();

@bind()
export default class Child extends Node {
    @bind.export(Variant.Type.TYPE_INT)
    accessor parentOnlyExport: number = 11;

    _ready() {
        console.log("Child ready");
    }

    childFn() {
        return true;
    }
}
