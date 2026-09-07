/**
 * Emscripten's modularized pthread shell uses this worker-global property to
 * reject loading the module more than once in the same worker realm.
 *
 * @type {boolean|undefined}
 */
globalThis.moduleLoaded;
