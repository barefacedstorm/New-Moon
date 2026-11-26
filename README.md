# New Moon (WebGPU / WebAssembly 2.0+ Edition)

A custom build of Pale Moon 35.1.0 / UXP extended with modern web platform APIs for Linux (x86_64):

## Added Capabilities
- **Native WebGPU (`navigator.gpu` & `canvas.getContext("webgpu")`)**: Backed by `wgpu-native` v29.0.1.1 (Vulkan), with triple-buffered non-blocking async presentation, non-blocking `GPUBuffer.mapAsync`, `GPUDevice : EventTarget`, and `SharedArrayBuffer`-backed `writeBuffer`/`writeTexture`.
- **WebAssembly 2.0+ SIMD & Threads**: Embedded `wasmtime` v51.0.0 (`libwasmtime.so`) with parallel Cranelift compilation, zero-copy `SharedArrayBuffer` shared memory, cross-Worker structured clone of `WebAssembly.Module` & shared `WebAssembly.Memory`, `WebAssembly.Module.customSections()`, and an `Atomics.notify()` -> Wasmtime `memory.atomic.notify` futex bridge.
- **Cross-Origin Isolation**: `window.crossOriginIsolated` / `self.crossOriginIsolated` (COOP/COEP) + `SharedArrayBuffer`.
- **Origin Private File System (OPFS)**: Disk-backed `navigator.storage.getDirectory()`, `createWritable()`, `createSyncAccessHandle()` (with shared-buffer views), and `FileSystemFileHandle.move()`.
- **ES Module Workers**: `new Worker(url, { type: "module" })` with `import.meta.url`, static `import`, and dynamic `import()`.
- **Web Audio `AudioWorklet` & `AudioWorkletNode`**: CSP-safe native module evaluation, `AudioWorkletProcessor`, `registerProcessor`, `MessageChannel` `port`, and 128-sample render quanta.
- **Modern Web/DOM Compatibility**:
  - `Blob.prototype.text()`, `arrayBuffer()`, `bytes()`
  - `ReadableStreamBYOBReader` EOF close fix
  - `WindowOrWorkerGlobalScope.reportError()`
  - CSS `:defined` pseudo-class

## Building on Linux (x86_64)
1. Install Pale Moon / UXP build dependencies (`build-essential`, `python2`, `libgtk-3-dev`, `libdbus-glib-1-dev`, `libpulse-dev`, `yasm`, `libx11-xcb-dev`, etc.).
2. Download the prebuilt C API shared libraries:
   - `wgpu-native` v29.0.1.1 (`libwgpu_native.so`) -> `~/palemoon-wgpu-native/lib/`
   - `wasmtime` v51.0.0 (`libwasmtime.so`) -> `~/palemoon-wasmtime/lib/`
3. Build:
   ```bash
   ./mach build
   ```
4. For smooth WebGPU canvas compositing on Linux, enable OpenGL layers (`pref("layers.acceleration.force-enabled", true);`).
