const base = require("./runtime-replace-base");

module.exports.getCombined = function getCombined() {
    return base.baseValue() + 1;
};
