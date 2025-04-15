
# GodotJS 
This project adds TypeScript/JavaScript Support for Godot 4.x by leveraging the high-performance capabilities of V8 to bring the delightful development experience of TypeScript into Godot. Meanwhile, it also supports switching to QuickJS, JavaScriptCore, or even directly run scripts on the host browser JS VM if porting to web.  

See [Breaking Changes](https://github.com/godotjs/GodotJS/wiki/Breaking-Changes) if upgrading from old versions.

[![windows](https://github.com/ialex32x/GodotJS-Build/actions/workflows/build_editor_windows.yml/badge.svg)](https://github.com/ialex32x/GodotJS-Build/actions/workflows/build_editor_windows.yml)
[![macos](https://github.com/ialex32x/GodotJS-Build/actions/workflows/build_editor_macos.yml/badge.svg)](https://github.com/ialex32x/GodotJS-Build/actions/workflows/build_editor_macos.yml)
[![linux](https://github.com/ialex32x/GodotJS-Build/actions/workflows/build_editor_linux.yml/badge.svg)](https://github.com/ialex32x/GodotJS-Build/actions/workflows/build_editor_linux.yml)

> [!NOTE]
> The core functionality is implemented and essentially usable but still under testing.  

![typescript_intellisence](https://github.com/godotjs/GodotJS/wiki/assets/typescript_intellisence.png)

## Features
* [x] Godot ScriptLanguage integration
* [x] Debug with Chrome/VSCode (with v8) and Safari (with JavaScriptCore)
* [x] REPL in Editor
* [x] Hot-reloading
* [x] Support for multiple javascript engines ([v8](https://github.com/v8/v8), [quickjs](https://github.com/bellard/quickjs), [quickjs-ng](https://github.com/quickjs-ng/quickjs), [JavaScriptCore](https://developer.apple.com/documentation/javascriptcore), the host Browser JS)
* [x] [Worker threads](https://github.com/godotjs/GodotJS/wiki/Worker) (limited support) (**experimental**)
* [x] Asynchronously loaded modules (limited support) (*temporarily in v8.impl only*)

## Get Started

Before initiating, make sure to select the JavaScript runtime you prefer between `v8`, `QuickJS` and `Web` (See [Supported Platforms](#supported-platforms)):

* `v8` is proven to be one of the most powerful and high-performance JavaScript runtimes.
* `QuickJS` is a remarkable and lightweight option.
* `JavaScriptCore` is the built-in JavaScript engine for WebKit and bundled with macOS/iOS.
* `Web` is only suitable when building for Web. All scripts run on the host browser JS VM rather than an additional interpreter.

See [Wiki](https://github.com/godotjs/GodotJS/wiki) for more details.


> [!NOTE]
> A prebuilt version of Godot Editor with GodotJS integrated can be downloaded from [GodotJS-Build](https://github.com/ialex32x/GodotJS-Build/releases).  
> The GodotJS-Build workflow is currently run manually, it may not be built from the latest version of GodotJS.

## Examples 

For more information on how to use `GodotJS` in a project, check out [GodotJSExample](https://github.com/ialex32x/GodotJSExample.git) for examples written in typescript.  
**And, don't forget to run `npm install` and `npx tsc` before opening the example project.**

[![Example: Snake](https://github.com/godotjs/GodotJS/wiki/assets/snake_01.gif)](https://github.com/ialex32x/GodotJSExample.git)
[![Example: Jummpy Bird](https://github.com/godotjs/GodotJS/wiki/assets/jumpybird.gif)](https://github.com/ialex32x/GodotJSExample.git)

## Supported Platforms

|                | v8                  | quickjs          | quickjs-ng            | Web Builtin JS      | JavaScriptCore    |
| -------------- | ------------------- | ---------------- | --------------------- | ------------------- | ----------------- |
| Windows:x86_64 | ✅                  | ✅              | ✅                     | ❌                  | ❌              |
| Windows:arm64  | ✅                  | ✅              | ✅                     | ❌                  | ❌              |
| MacOS:x86_64   | ✅ (not tested)     | ✅ (not tested) | ✅ (not tested)        | ❌                  | ✅ (not tested) |
| MacOS:arm64    | ✅                  | ✅              | ✅                     | ❌                  | ✅ (debugging)  |
| Linux:x86_64   | ✅ (not tested)     | ✅ (not tested) | ✅                     | ❌                  | ❌              |
| Linux:arm64    | ✅                  | ✅              | ✅                     | ❌                  | ❌              |
| Android:x86_64 | ✅ (not tested)     | ✅ (not tested) | ✅ (not tested)        | ❌                  | ❌              |
| Android:arm64  | ✅                  | ✅ (not tested) | ✅ (not tested)        | ❌                  | ❌              |
| iOS:x86_64     | ✅ (not tested)     | ✅ (not tested) | ✅ (not tested)        | ❌                  | ✅ (not tested) |
| iOS:arm64      | ✅ (not tested)     | ✅ (not tested) | ✅ (not tested)        | ❌                  | ✅ (not tested) |
| Web:wasm32     | ❌                  | ✅ (not tested) | ✅ (not tested)        | ✅ (debugging)      | ❌              |
| Debugger       | ✅ Chrome/VSCode    | ❌              | ❌                     | ✅ browser devtools | ✅ Safari       |


> Android: only tested on ndk_platform=android-24  
> Web: only tested on emsdk-3.1.64  
> JavaScriptCore: macOS 15, iOS 18 (support for lower versions may be implemented in future versions)  



