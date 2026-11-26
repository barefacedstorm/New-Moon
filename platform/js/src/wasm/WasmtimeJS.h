/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_wasmtime_js_h
#define wasm_wasmtime_js_h

#include "jsapi.h"
#include "vm/NativeObject.h"

struct JSStructuredCloneWriter;
struct JSStructuredCloneReader;

namespace js {
namespace wasm {

// Initializes the WebAssembly global object backed by libwasmtime.
JSObject* InitWasmtimeWebAssemblyClass(JSContext* cx, HandleObject global);

// Structured clone helpers for cross-Worker postMessage(module) / postMessage(memory).
bool IsWasmtimeModuleObject(JSObject* obj);
bool IsWasmtimeSharedMemoryObject(JSObject* obj);

bool WriteWasmtimeModule(JSContext* cx, HandleObject obj, void** outClonedModule);
JSObject* ReadWasmtimeModule(JSContext* cx, void* clonedModule);

bool WriteWasmtimeSharedMemory(JSContext* cx, HandleObject obj,
                               void** outClonedShmem, void** outRawBuf, uint32_t* outMaxPages);
JSObject* ReadWasmtimeSharedMemory(JSContext* cx, void* clonedShmem,
                                   void* rawBuf, uint32_t maxPages);

// Bridge SpiderMonkey Atomics.notify() -> Wasmtime memory.atomic.notify().
int32_t NotifyWasmtimeSharedMemory(void* externalOwner, uint32_t byteOffset, uint32_t count);

} // namespace wasm
} // namespace js

#endif // wasm_wasmtime_js_h
