#!/usr/bin/env node

"use strict";

const { exportWebCommonJS } = require("./export-web-commonjs");
const {
    defaultReadySentinel,
    defaultSmokeScriptResPath,
    runExportUnderNode,
    validateExportShape,
} = require("./run-web-commonjs-export-under-node");

async function main() {
    const exportDir = exportWebCommonJS();
    validateExportShape({ exportDir });
    await runExportUnderNode({ exportDir });
    console.log(`PASS ${defaultReadySentinel} (${defaultSmokeScriptResPath})`);
}

main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
