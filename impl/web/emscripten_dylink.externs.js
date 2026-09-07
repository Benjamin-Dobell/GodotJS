/**
 * @externs
 *
 * Emscripten dynamic side modules can contain EM_ASM source that is evaluated
 * only after the main module's Closure pass. Keep the runtime names used by
 * those callbacks stable across the main/side compilation boundary.
 */

/**
 * @type {!Object}
 * @suppress {duplicate}
 */
var FS;

/**
 * @param {boolean} populate
 * @param {function(*): void} callback
 * @return {void}
 */
FS.syncfs = function(populate, callback) {};

/**
 * @param {string} value
 * @return {number}
 * @suppress {duplicate}
 */
function lengthBytesUTF8(value) {}

/**
 * @param {number} size
 * @return {number}
 * @suppress {duplicate}
 */
function _malloc(size) {}

/**
 * @param {string} value
 * @param {number} output
 * @param {number} maximumBytes
 * @return {void}
 * @suppress {duplicate}
 */
function stringToUTF8(value, output, maximumBytes) {}

/**
 * @param {number} pointer
 * @return {function(...*): *}
 * @suppress {duplicate}
 */
function getWasmTableEntry(pointer) {}

/**
 * @param {number} pointer
 * @return {void}
 * @suppress {duplicate}
 */
function _free(pointer) {}
