# New Moon (WebGPU & WebAssembly 2.0+ Edition)

**New Moon** is a custom technical build of **Pale Moon 35.1.0** (Unified XUL Platform / UXP) for **Linux x86_64**, extended with modern graphics, compute, storage, audio, and web-platform APIs while preserving the classic lightweight XUL browser architecture.

---

## Why New Moon?

Stock Pale Moon / UXP lacks several modern web standards required by WebGPU applications, multi-threaded WebAssembly 2.0 games, and modern web apps (such as `nfsu2.xyz` and GitHub). New Moon bridges those gaps natively in C++ by embedding **wgpu-native** (Vulkan) and **Wasmtime** directly into `libxul.so`.

---

## Technical Architecture & Added Capabilities

| Subsystem | Implementation | Key Source Files |
| :--- | :--- | :--- |
| **WebGPU (`navigator.gpu`, `canvas.getContext("webgpu")`)** | Native bindings to **wgpu-native v29.0.1.1** (Vulkan backend). Includes triple-buffered non-blocking async canvas presentation, non-blocking `GPUBuffer.mapAsync()` polling, `GPUDevice : EventTarget` (`uncapturederror`), and `SharedArrayBuffer`-backed `queue.writeBuffer()` / `queue.writeTexture()`. | `platform/dom/webidl/WebGPU.webidl`<br>`platform/dom/webgpu/{WebGPU.h,WebGPUHelpers.h,WebGPU.cpp,GPUCanvasContext.cpp}` |
| **WebAssembly 2.0+ (SIMD & Threads)** | Embedded **Wasmtime v51.0.0** (`libwasmtime.so`) replacing SpiderMonkey's legacy Wasm engine. Enables SIMD, Relaxed SIMD, Threads, Bulk Memory, Multi-Value, Reference Types, parallel Cranelift compilation, zero-copy `SharedArrayBuffer` linear memory, `WebAssembly.Module.customSections()`, and cross-Worker `postMessage()` structured cloning of `Module` and shared `Memory`. | `platform/js/src/wasm/{WasmtimeJS.h,WasmtimeJS.cpp}`<br>`platform/js/src/vm/{SharedArrayObject.h,SharedArrayObject.cpp,StructuredClone.cpp}` |
| **JS ↔ Wasmtime Futex Bridge** | Bridges SpiderMonkey's `Atomics.notify()` directly into Wasmtime's `memory.atomic.notify` via an internal helper module so JS main-thread wakeups unblock Wasmtime worker threads sleeping in `memory.atomic.wait32/wait64` with sub-millisecond latency. | `platform/js/src/builtin/AtomicsObject.cpp`<br>`platform/js/src/wasm/WasmtimeJS.cpp` |
| **Cross-Origin Isolation** | Implements `window.crossOriginIsolated` and `self.crossOriginIsolated` from HTTP `Cross-Origin-Opener-Policy` (`same-origin`) and `Cross-Origin-Embedder-Policy` (`require-corp` / `credentialless`) headers, unlocking `SharedArrayBuffer` and high-resolution timers. | `platform/dom/webidl/WindowOrWorkerGlobalScope.webidl`<br>`platform/dom/base/nsGlobalWindow.cpp`<br>`platform/dom/workers/{WorkerPrivate.cpp,WorkerScope.cpp}` |
| **Origin Private File System (OPFS)** | Disk-backed `navigator.storage.getDirectory()`, `persisted()`, `persist()`, `FileSystemDirectoryHandle`, `FileSystemFileHandle` (including `.move()`), `FileSystemWritableFileStream`, and Worker `FileSystemSyncAccessHandle` (supporting `SharedArrayBuffer` views). Data is stored per-origin under `<profile>/storage/opfs/`. | `platform/dom/webidl/OPFS.webidl`<br>`platform/dom/quota/{OPFS.h,OPFS.cpp,StorageManager.cpp}` |
| **ES Module Workers** | Supports `new Worker(url, { type: "module", name })` with `import.meta.url`, static `import`, dynamic `import()`, and a cycle-collector-safe script private guard. | `platform/dom/webidl/Worker.webidl`<br>`platform/dom/workers/{WorkerPrivate.cpp,ScriptLoader.cpp}`<br>`platform/xpcom/base/CycleCollectedJSContext.cpp` |
| **Web Audio `AudioWorklet`** | Exposes `AudioContext.audioWorklet` (`addModule()`), `AudioWorkletProcessor`, `registerProcessor()`, live `sampleRate`/`currentFrame`/`currentTime`, and `AudioWorkletNode` with `MessageChannel` `port` (supporting `SharedArrayBuffer` ring buffers) and 128-sample render quanta. Uses native C++ `JS::Evaluate()` so it works under strict Content-Security-Policy (no `'unsafe-eval'`). | `platform/dom/webidl/AudioContext.webidl`<br>`platform/dom/media/webaudio/AudioContext.{h,cpp}` |
| **Modern Web / DOM / CSS Fixes** | - `Blob.prototype.text()`, `arrayBuffer()`, `bytes()`<br>- `ReadableStreamBYOBReader` EOF close fix (resolves pending `readIntoRequests` on stream close)<br>- `WindowOrWorkerGlobalScope.reportError()`<br>- CSS `:defined` pseudo-class (including `:not(:defined)`) | `platform/dom/base/File.{h,cpp}`<br>`platform/js/src/builtin/Stream.cpp`<br>`platform/dom/base/nsGlobalWindow.cpp`<br>`platform/layout/style/{nsCSSPseudoClassList.h,nsCSSRuleUtils.cpp}` |

---

## System Requirements

- **OS**: Linux x86_64 (tested on Pop!_OS 24.04 LTS / Ubuntu 24.04)
- **GPU**: Vulkan-capable GPU with working Vulkan ICD (tested on NVIDIA GeForce RTX 5060 Laptop GPU)
- **RAM**: 8 GiB+ recommended (16 GiB+ for building from source)
- **Runtime Shared Libraries**:
  - `libwgpu_native.so` (from `wgpu-native` v29.0.1.1 Linux x86_64 release)
  - `libwasmtime.so` (from `wasmtime` v51.0.0 x86_64 Linux C API release)

---

## Known Limitations & Design Notes

1. **WebAssembly Memory Limit (< 2 GiB)**: SpiderMonkey 52's `TypedArray` and `SharedArrayBuffer` length slots are signed 32-bit (`INT32_MAX` = 2,147,483,647 bytes). `WebAssembly.Memory` enforces a maximum of **32,767 pages (~2047.9 MB)** and throws `RangeError` for `initial > 32767` so applications with a memory fallback ladder step down cleanly.
2. **WebGPU Canvas Presentation**: Uses a triple-buffered non-blocking Vulkan-to-surface readback ring rather than a direct child X11 Vulkan swapchain. For smooth 60 FPS presentation at high resolutions, enable OpenGL hardware compositing:
   ```js
   pref("layers.acceleration.force-enabled", true);
   ```
3. **`AudioWorklet` Execution Model**: Runs 128-sample Web Audio render quanta on top of UXP's `ScriptProcessorNode` (1024-sample buffer, ~21 ms latency at 48 kHz) rather than a separate dedicated audio worklet JS thread.
4. **Linux x86_64 Focus**: Build configuration and `RUNPATH` settings in `platform/toolkit/library/moz.build` target Linux x86_64.

---

## Building from Source (Ubuntu / Pop!_OS 24.04)

### 1. Install Build Dependencies
```bash
sudo apt update
sudo apt install -y \
  build-essential git autoconf2.13 yasm zip unzip pkg-config \
  libgtk-3-dev libdbus-glib-1-dev libpulse-dev libasound2-dev \
  libxt-dev libx11-xcb-dev libegl1-mesa-dev libgles2-mesa-dev \
  libgconf2-dev python2
```
*(If `libgconf2-dev` is unavailable on Ubuntu 24.04, `.mozconfig` already disables GConf.)*

### 2. Fetch Prebuilt `wgpu-native` and `wasmtime` Libraries
Place the shared libraries where `platform/toolkit/library/moz.build` expects them (or update the paths in `moz.build` / `.mozconfig`):

```bash
# wgpu-native v29.0.1.1 (Linux x86_64 release)
mkdir -p ~/palemoon-wgpu-native
# Extract lib/libwgpu_native.so into ~/palemoon-wgpu-native/lib/

# wasmtime v51.0.0 C API (x86_64-linux)
mkdir -p ~/palemoon-wasmtime
# Extract lib/libwasmtime.so into ~/palemoon-wasmtime/lib/
```
*(The C headers are already vendored in-tree under `platform/dom/webgpu/include` and `platform/js/src/wasm/wasmtime_include`.)*

### 3. Build & Run
```bash
./mach build
```
To stage a standalone runnable directory alongside `libwgpu_native.so` and `libwasmtime.so`:
```bash
./mach package
```
Copy `libwgpu_native.so` and `libwasmtime.so` next to `palemoon` and `libxul.so` (the binary's `RUNPATH` includes `$ORIGIN`).

---

## License & Attribution

- Based on **Pale Moon 35.1.0** and the **Unified XUL Platform (UXP)**, licensed under the **Mozilla Public License 2.0 (MPL-2.0)** (see `LICENSE`).
- Uses **[wgpu-native](https://github.com/gfx-rs/wgpu-native)** (Apache-2.0 / MIT) and **[Wasmtime](https://github.com/bytecodealliance/wasmtime)** (Apache-2.0 with LLVM exception).
- This is an unofficial community build and is not affiliated with or endorsed by Moonchild Productions.
