/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmtimeJS.h"
#include "wasm/WasmJS.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jscntxt.h"
#include "jsobj.h"
#include "jsobjinlines.h"
#include "builtin/Promise.h"
#include "vm/ArrayBufferObject.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/SharedArrayObject.h"

#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif
#include <wasmtime.h>
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace js;

namespace js {
namespace wasm {

namespace {

// ============================================================================
// Global Engine & Per-Thread Store
// ============================================================================

static std::mutex gEngineMutex;
static wasm_engine_t* gWasmtimeEngine = nullptr;

static wasm_engine_t*
GetGlobalEngine()
{
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gWasmtimeEngine) {
        wasm_config_t* cfg = wasm_config_new();
        wasmtime_config_wasm_simd_set(cfg, true);
        wasmtime_config_wasm_relaxed_simd_set(cfg, true);
        wasmtime_config_wasm_threads_set(cfg, true);
        wasmtime_config_shared_memory_set(cfg, true);
        wasmtime_config_wasm_bulk_memory_set(cfg, true);
        wasmtime_config_wasm_multi_value_set(cfg, true);
        wasmtime_config_wasm_reference_types_set(cfg, true);
        wasmtime_config_parallel_compilation_set(cfg, true);
        wasmtime_config_cranelift_opt_level_set(cfg, WASMTIME_OPT_LEVEL_SPEED);
        gWasmtimeEngine = wasm_engine_new_with_config(cfg);
    }
    return gWasmtimeEngine;
}

struct ThreadStore {
    wasmtime_store_t* store = nullptr;
    wasmtime_context_t* ctx = nullptr;
    JSContext* activeCx = nullptr;

    ~ThreadStore() {
        if (store) {
            wasmtime_store_delete(store);
            store = nullptr;
            ctx = nullptr;
        }
    }
};

static thread_local ThreadStore gThreadStore;

static ThreadStore&
GetThreadStore()
{
    if (!gThreadStore.store) {
        gThreadStore.store = wasmtime_store_new(GetGlobalEngine(), nullptr, nullptr);
        gThreadStore.ctx = wasmtime_store_context(gThreadStore.store);
    }
    return gThreadStore;
}

static wasmtime_context_t*
GetStoreCtx()
{
    return GetThreadStore().ctx;
}

struct AutoActiveCx {
    JSContext* prev;
    explicit AutoActiveCx(JSContext* cx) : prev(GetThreadStore().activeCx) {
        GetThreadStore().activeCx = cx;
    }
    ~AutoActiveCx() {
        GetThreadStore().activeCx = prev;
    }
};

// ============================================================================
// Error helpers
// ============================================================================

static std::string
TakeWasmtimeError(wasmtime_error_t* err)
{
    wasm_name_t msg;
    wasmtime_error_message(err, &msg);
    std::string s(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return s;
}

static std::string
TakeTrapMessage(wasm_trap_t* trap)
{
    wasm_name_t msg;
    wasm_trap_message(trap, &msg);
    std::string s(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasm_trap_delete(trap);
    return s;
}

static void
ThrowCompileError(JSContext* cx, const std::string& msg)
{
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_COMPILE_ERROR, msg.c_str());
}

static void
ThrowLinkError(JSContext* cx, const std::string& msg)
{
    JS_ReportErrorUTF8(cx, "WebAssembly.LinkError: %s", msg.c_str());
}

static void
ThrowRuntimeError(JSContext* cx, const std::string& msg)
{
    JS_ReportErrorUTF8(cx, "WebAssembly.RuntimeError: %s", msg.c_str());
}

// ============================================================================
// BufferSource extraction
// ============================================================================

static bool
ExtractBytes(JSContext* cx, HandleValue val, std::vector<uint8_t>& out)
{
    if (!val.isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly argument must be a BufferSource");
        return false;
    }
    RootedObject obj(cx, &val.toObject());
    if (JS_IsArrayBufferObject(obj)) {
        uint32_t len = JS_GetArrayBufferByteLength(obj);
        JS::AutoCheckCannotGC nogc;
        bool isShared = false;
        uint8_t* data = JS_GetArrayBufferData(obj, &isShared, nogc);
        if (len > 0 && !data) {
            JS_ReportErrorASCII(cx, "Detached ArrayBuffer");
            return false;
        }
        out.assign(data, data + len);
        return true;
    }
    if (JS_IsArrayBufferViewObject(obj)) {
        uint32_t len = JS_GetArrayBufferViewByteLength(obj);
        JS::AutoCheckCannotGC nogc;
        bool isShared = false;
        uint8_t* data = static_cast<uint8_t*>(JS_GetArrayBufferViewData(obj, &isShared, nogc));
        if (len > 0 && !data) {
            JS_ReportErrorASCII(cx, "Detached ArrayBufferView");
            return false;
        }
        out.assign(data, data + len);
        return true;
    }
    JS_ReportErrorASCII(cx, "WebAssembly argument must be an ArrayBuffer or ArrayBufferView");
    return false;
}

// ============================================================================
// Forward declarations of wrappers
// ============================================================================

static JSObject* CreateExportedFunc(JSContext* cx, const wasmtime_func_t& func, HandleObject keepAlive);
static JSObject* WrapMemory(JSContext* cx, const wasmtime_memory_t& mem, uint32_t maxPages);
static JSObject* WrapSharedMemory(JSContext* cx, wasmtime_sharedmemory_t* shmem, SharedArrayRawBuffer* rawBuf, uint32_t maxPages);
static JSObject* WrapTable(JSContext* cx, const wasmtime_table_t& table, bool isFuncRef);
static JSObject* WrapGlobal(JSContext* cx, const wasmtime_global_t& global, wasm_valkind_t kind, bool isMutable);

static bool IsExportedWasmtimeFunc(JSObject* obj, wasmtime_func_t* outFunc);

// ============================================================================
// Value conversions JS <-> Wasmtime
// ============================================================================

static bool
JSToWasmVal(JSContext* cx, HandleValue v, wasm_valkind_t kind, wasmtime_val_t& out)
{
    memset(&out, 0, sizeof(out));
    switch (kind) {
      case WASM_I32: {
        int32_t i = 0;
        if (!JS::ToInt32(cx, v, &i)) return false;
        out.kind = WASMTIME_I32;
        out.of.i32 = i;
        return true;
      }
      case WASM_I64: {
        out.kind = WASMTIME_I64;
        if (v.isBigInt()) {
            out.of.i64 = BigInt::toInt64(v.toBigInt());
            return true;
        }
        BigInt* bi = js::ToBigInt(cx, v);
        if (bi) {
            out.of.i64 = BigInt::toInt64(bi);
            return true;
        }
        JS_ClearPendingException(cx);
        double d = 0;
        if (!JS::ToNumber(cx, v, &d)) return false;
        out.of.i64 = static_cast<int64_t>(d);
        return true;
      }
      case WASM_F32: {
        double d = 0;
        if (!JS::ToNumber(cx, v, &d)) return false;
        out.kind = WASMTIME_F32;
        out.of.f32 = static_cast<float>(d);
        return true;
      }
      case WASM_F64: {
        double d = 0;
        if (!JS::ToNumber(cx, v, &d)) return false;
        out.kind = WASMTIME_F64;
        out.of.f64 = d;
        return true;
      }
      case WASM_FUNCREF: {
        out.kind = WASMTIME_FUNCREF;
        out.of.funcref.store_id = 0;
        if (v.isObject()) {
            wasmtime_func_t f;
            if (IsExportedWasmtimeFunc(&v.toObject(), &f)) {
                out.of.funcref = f;
            }
        }
        return true;
      }
      default: {
        out.kind = WASMTIME_I32;
        out.of.i32 = 0;
        return true;
      }
    }
}

static bool
WasmValToJS(JSContext* cx, const wasmtime_val_t& v, MutableHandleValue out)
{
    switch (v.kind) {
      case WASMTIME_I32:
        out.setInt32(v.of.i32);
        return true;
      case WASMTIME_I64: {
        BigInt* bi = BigInt::createFromInt64(cx, v.of.i64);
        if (!bi) return false;
        out.setBigInt(bi);
        return true;
      }
      case WASMTIME_F32:
        out.setNumber(JS::CanonicalizeNaN(static_cast<double>(v.of.f32)));
        return true;
      case WASMTIME_F64:
        out.setNumber(JS::CanonicalizeNaN(v.of.f64));
        return true;
      case WASMTIME_FUNCREF: {
        if (v.of.funcref.store_id == 0) {
            out.setNull();
            return true;
        }
        JSObject* fn = CreateExportedFunc(cx, v.of.funcref, nullptr);
        if (!fn) return false;
        out.setObject(*fn);
        return true;
      }
      default:
        out.setUndefined();
        return true;
    }
}

static const char*
ExternKindToString(wasm_externkind_t k)
{
    switch (k) {
      case WASM_EXTERN_FUNC: return "function";
      case WASM_EXTERN_GLOBAL: return "global";
      case WASM_EXTERN_TABLE: return "table";
      case WASM_EXTERN_MEMORY: return "memory";
      default: return "unknown";
    }
}

// ============================================================================
// Module Object
// ============================================================================

struct WasmtimeModuleData {
    wasmtime_module_t* module;
    std::vector<uint8_t> rawBytes; // for customSections()
};

static void
WasmtimeModule_finalize(FreeOp* fop, JSObject* obj)
{
    auto* data = static_cast<WasmtimeModuleData*>(JS_GetReservedSlot(obj, 0).toPrivate());
    if (data) {
        if (data->module) {
            wasmtime_module_delete(data->module);
        }
        delete data;
    }
}

static const ClassOps WasmtimeModuleClassOps = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    WasmtimeModule_finalize
};

static const Class WasmtimeModuleClass = {
    "Module",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
    &WasmtimeModuleClassOps
};

static JSObject*
WrapWasmtimeModule(JSContext* cx, wasmtime_module_t* mod, std::vector<uint8_t>&& rawBytes)
{
    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmModule).toObject());
    RootedObject obj(cx, JS_NewObjectWithGivenProto(cx, Jsvalify(&WasmtimeModuleClass), proto));
    if (!obj) {
        wasmtime_module_delete(mod);
        return nullptr;
    }
    auto* data = new WasmtimeModuleData{ mod, std::move(rawBytes) };
    JS_SetReservedSlot(obj, 0, PrivateValue(data));
    return obj;
}

static WasmtimeModuleData*
UnwrapWasmtimeModule(JSObject* obj)
{
    if (!obj) return nullptr;
    obj = CheckedUnwrap(obj);
    if (!obj || JS_GetClass(obj) != Jsvalify(&WasmtimeModuleClass)) return nullptr;
    return static_cast<WasmtimeModuleData*>(JS_GetReservedSlot(obj, 0).toPrivate());
}

static bool
WasmtimeModule_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.isConstructing()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Module must be called with new");
        return false;
    }
    if (args.length() < 1) {
        JS_ReportErrorASCII(cx, "WebAssembly.Module requires a BufferSource argument");
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ExtractBytes(cx, args[0], bytes)) return false;

    wasmtime_module_t* mod = nullptr;
    wasmtime_error_t* err = wasmtime_module_new(GetGlobalEngine(), bytes.data(), bytes.size(), &mod);
    if (err) {
        ThrowCompileError(cx, TakeWasmtimeError(err));
        return false;
    }
    JSObject* obj = WrapWasmtimeModule(cx, mod, std::move(bytes));
    if (!obj) return false;
    args.rval().setObject(*obj);
    return true;
}

static bool
WasmtimeModule_imports(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Module.imports requires a Module");
        return false;
    }
    auto* md = UnwrapWasmtimeModule(&args[0].toObject());
    if (!md) {
        JS_ReportErrorASCII(cx, "Argument is not a WebAssembly.Module");
        return false;
    }

    wasm_importtype_vec_t imports;
    wasmtime_module_imports(md->module, &imports);

    RootedObject arr(cx, JS_NewArrayObject(cx, imports.size));
    if (!arr) {
        wasm_importtype_vec_delete(&imports);
        return false;
    }

    for (size_t i = 0; i < imports.size; ++i) {
        const wasm_importtype_t* it = imports.data[i];
        const wasm_name_t* mname = wasm_importtype_module(it);
        const wasm_name_t* fname = wasm_importtype_name(it);
        const wasm_externtype_t* et = wasm_importtype_type(it);
        const char* kind = ExternKindToString(wasm_externtype_kind(et));

        RootedObject entry(cx, JS_NewPlainObject(cx));
        RootedString sMod(cx, JS_NewStringCopyN(cx, mname->data, mname->size));
        RootedString sName(cx, JS_NewStringCopyN(cx, fname->data, fname->size));
        RootedString sKind(cx, JS_NewStringCopyZ(cx, kind));
        if (!entry || !sMod || !sName || !sKind) {
            wasm_importtype_vec_delete(&imports);
            return false;
        }
        RootedValue v(cx);
        v.setString(sMod);  JS_DefineProperty(cx, entry, "module", v, JSPROP_ENUMERATE);
        v.setString(sName); JS_DefineProperty(cx, entry, "name", v, JSPROP_ENUMERATE);
        v.setString(sKind); JS_DefineProperty(cx, entry, "kind", v, JSPROP_ENUMERATE);
        v.setObject(*entry);
        JS_DefineElement(cx, arr, i, v, JSPROP_ENUMERATE);
    }

    wasm_importtype_vec_delete(&imports);
    args.rval().setObject(*arr);
    return true;
}

static bool
WasmtimeModule_exports(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Module.exports requires a Module");
        return false;
    }
    auto* md = UnwrapWasmtimeModule(&args[0].toObject());
    if (!md) {
        JS_ReportErrorASCII(cx, "Argument is not a WebAssembly.Module");
        return false;
    }

    wasm_exporttype_vec_t exports;
    wasmtime_module_exports(md->module, &exports);

    RootedObject arr(cx, JS_NewArrayObject(cx, exports.size));
    if (!arr) {
        wasm_exporttype_vec_delete(&exports);
        return false;
    }

    for (size_t i = 0; i < exports.size; ++i) {
        const wasm_exporttype_t* et = exports.data[i];
        const wasm_name_t* ename = wasm_exporttype_name(et);
        const wasm_externtype_t* xt = wasm_exporttype_type(et);
        const char* kind = ExternKindToString(wasm_externtype_kind(xt));

        RootedObject entry(cx, JS_NewPlainObject(cx));
        RootedString sName(cx, JS_NewStringCopyN(cx, ename->data, ename->size));
        RootedString sKind(cx, JS_NewStringCopyZ(cx, kind));
        if (!entry || !sName || !sKind) {
            wasm_exporttype_vec_delete(&exports);
            return false;
        }
        RootedValue v(cx);
        v.setString(sName); JS_DefineProperty(cx, entry, "name", v, JSPROP_ENUMERATE);
        v.setString(sKind); JS_DefineProperty(cx, entry, "kind", v, JSPROP_ENUMERATE);
        v.setObject(*entry);
        JS_DefineElement(cx, arr, i, v, JSPROP_ENUMERATE);
    }

    wasm_exporttype_vec_delete(&exports);
    args.rval().setObject(*arr);
    return true;
}

static bool
ReadULEB128(const uint8_t* data, size_t len, size_t& pos, uint32_t& out)
{
    uint32_t result = 0;
    unsigned shift = 0;
    while (pos < len) {
        uint8_t b = data[pos++];
        result |= uint32_t(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
        if (shift >= 35) return false;
    }
    return false;
}

static bool
WasmtimeModule_customSections(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 2 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Module.customSections requires (module, sectionName)");
        return false;
    }
    auto* md = UnwrapWasmtimeModule(&args[0].toObject());
    if (!md) {
        JS_ReportErrorASCII(cx, "First argument is not a WebAssembly.Module");
        return false;
    }
    RootedString nameStr(cx, JS::ToString(cx, args[1]));
    if (!nameStr) return false;
    JSAutoByteString nameUtf8(cx, nameStr);
    if (!nameUtf8) return false;
    const char* targetName = nameUtf8.ptr();
    size_t targetLen = strlen(targetName);

    RootedObject arr(cx, JS_NewArrayObject(cx, 0));
    if (!arr) return false;

    const uint8_t* data = md->rawBytes.data();
    size_t len = md->rawBytes.size();
    uint32_t outIdx = 0;

    if (len >= 8) {
        size_t pos = 8; // skip \0asm + version
        while (pos < len) {
            uint8_t secId = data[pos++];
            uint32_t secSize = 0;
            if (!ReadULEB128(data, len, pos, secSize) || pos + secSize > len) {
                break;
            }
            size_t secEnd = pos + secSize;
            if (secId == 0) {
                size_t p = pos;
                uint32_t nLen = 0;
                if (ReadULEB128(data, secEnd, p, nLen) && p + nLen <= secEnd) {
                    if (nLen == targetLen && memcmp(data + p, targetName, targetLen) == 0) {
                        size_t payloadOff = p + nLen;
                        size_t payloadLen = secEnd - payloadOff;
                        RootedObject ab(cx, JS_NewArrayBuffer(cx, static_cast<uint32_t>(payloadLen)));
                        if (!ab) return false;
                        if (payloadLen > 0) {
                            JS::AutoCheckCannotGC nogc;
                            bool isShared = false;
                            uint8_t* dst = JS_GetArrayBufferData(ab, &isShared, nogc);
                            memcpy(dst, data + payloadOff, payloadLen);
                        }
                        RootedValue v(cx, ObjectValue(*ab));
                        if (!JS_DefineElement(cx, arr, outIdx++, v, JSPROP_ENUMERATE)) return false;
                    }
                }
            }
            pos = secEnd;
        }
    }

    args.rval().setObject(*arr);
    return true;
}

// ============================================================================
// Memory Object (Unshared & Shared)
// ============================================================================

struct WasmtimeMemoryData {
    bool isShared = false;
    wasmtime_memory_t mem = {};
    wasmtime_sharedmemory_t* shmem = nullptr;
    SharedArrayRawBuffer* sharedRawBuf = nullptr;
    uint32_t maxPages = 65536;
};

struct WasmtimeSharedMemoryOwner {
    wasmtime_sharedmemory_t* shmem = nullptr;
    std::mutex notifyMutex;
    wasmtime_store_t* notifyStore = nullptr;
    wasmtime_context_t* notifyCtx = nullptr;
    wasmtime_func_t notifyFunc = {};
    bool notifyReady = false;
    bool notifyFailed = false;
};

static std::mutex gNotifyModMutex;
static wasmtime_module_t* gNotifyHelperMod = nullptr;

static wasmtime_module_t*
GetNotifyHelperModule()
{
    std::lock_guard<std::mutex> lock(gNotifyModMutex);
    if (!gNotifyHelperMod) {
        // (module
        //   (import "e" "m" (memory 0 65536 shared))
        //   (func (export "n") (param i32 i32) (result i32)
        //     local.get 0
        //     local.get 1
        //     memory.atomic.notify align=2 offset=0))
        static const uint8_t kNotifyWasm[] = {
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
            0x02, 0x0b, 0x01, 0x01, 0x65, 0x01, 0x6d, 0x02, 0x03, 0x00, 0x80, 0x80, 0x04,
            0x03, 0x02, 0x01, 0x00,
            0x07, 0x05, 0x01, 0x01, 0x6e, 0x00, 0x00,
            0x0a, 0x0c, 0x01, 0x0a, 0x00,
            0x20, 0x00,
            0x20, 0x01,
            0xfe, 0x00, 0x02, 0x00,
            0x0b
        };
        wasmtime_error_t* err = wasmtime_module_new(
            GetGlobalEngine(), kNotifyWasm, sizeof(kNotifyWasm), &gNotifyHelperMod);
        if (err) {
            wasmtime_error_delete(err);
            gNotifyHelperMod = nullptr;
        }
    }
    return gNotifyHelperMod;
}

static void
DropWasmtimeSharedMemory(void* ownerPtr)
{
    auto* owner = static_cast<WasmtimeSharedMemoryOwner*>(ownerPtr);
    if (owner) {
        if (owner->notifyStore) {
            wasmtime_store_delete(owner->notifyStore);
            owner->notifyStore = nullptr;
        }
        if (owner->shmem) {
            wasmtime_sharedmemory_delete(owner->shmem);
            owner->shmem = nullptr;
        }
        delete owner;
    }
}

static void
WasmtimeMemory_finalize(FreeOp* fop, JSObject* obj)
{
    auto* data = static_cast<WasmtimeMemoryData*>(JS_GetReservedSlot(obj, 0).toPrivate());
    if (data) {
        if (data->sharedRawBuf) {
            data->sharedRawBuf->dropReference();
        }
        if (data->shmem) {
            wasmtime_sharedmemory_delete(data->shmem);
        }
        delete data;
    }
}

static const ClassOps WasmtimeMemoryClassOps = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    WasmtimeMemory_finalize
};

static const Class WasmtimeMemoryClass = {
    "Memory",
    JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_FOREGROUND_FINALIZE,
    &WasmtimeMemoryClassOps
};

static WasmtimeMemoryData*
UnwrapWasmtimeMemory(JSObject* obj)
{
    if (!obj) return nullptr;
    obj = CheckedUnwrap(obj);
    if (!obj || JS_GetClass(obj) != Jsvalify(&WasmtimeMemoryClass)) return nullptr;
    return static_cast<WasmtimeMemoryData*>(JS_GetReservedSlot(obj, 0).toPrivate());
}

static JSObject*
WrapMemory(JSContext* cx, const wasmtime_memory_t& mem, uint32_t maxPages)
{
    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());
    RootedObject obj(cx, JS_NewObjectWithGivenProto(cx, Jsvalify(&WasmtimeMemoryClass), proto));
    if (!obj) return nullptr;
    auto* data = new WasmtimeMemoryData();
    data->isShared = false;
    data->mem = mem;
    data->maxPages = maxPages;
    JS_SetReservedSlot(obj, 0, PrivateValue(data));
    JS_SetReservedSlot(obj, 1, UndefinedValue());
    return obj;
}

static JSObject*
WrapSharedMemory(JSContext* cx, wasmtime_sharedmemory_t* shmem, SharedArrayRawBuffer* rawBuf, uint32_t maxPages)
{
    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());
    RootedObject obj(cx, JS_NewObjectWithGivenProto(cx, Jsvalify(&WasmtimeMemoryClass), proto));
    if (!obj) return nullptr;

    if (!rawBuf) {
        uint8_t* base = wasmtime_sharedmemory_data(shmem);
        size_t byteLen = wasmtime_sharedmemory_data_size(shmem);
        uint32_t maxBytes = (maxPages > 32767) ? 0x7FFF0000u : (maxPages * 65536u);
        if (maxBytes < byteLen) maxBytes = static_cast<uint32_t>(byteLen);
        auto* bufOwner = new WasmtimeSharedMemoryOwner();
        bufOwner->shmem = wasmtime_sharedmemory_clone(shmem);
        rawBuf = SharedArrayRawBuffer::NewExternal(
            base, static_cast<uint32_t>(byteLen), maxBytes, true,
            bufOwner, DropWasmtimeSharedMemory);
        if (!rawBuf) {
            DropWasmtimeSharedMemory(bufOwner);
            return nullptr;
        }
    } else {
        rawBuf->addReference();
    }

    auto* data = new WasmtimeMemoryData();
    data->isShared = true;
    data->shmem = shmem;
    data->sharedRawBuf = rawBuf;
    data->maxPages = maxPages;
    JS_SetReservedSlot(obj, 0, PrivateValue(data));

    // Create the SharedArrayBuffer JS wrapper referencing rawBuf.
    rawBuf->addReference();
    RootedObject sab(cx, SharedArrayBufferObject::New(cx, rawBuf));
    if (!sab) {
        rawBuf->dropReference();
        return nullptr;
    }
    JS_SetReservedSlot(obj, 1, ObjectValue(*sab));
    return obj;
}

static bool
WasmtimeMemory_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.isConstructing() || args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Memory requires a descriptor object");
        return false;
    }
    RootedObject desc(cx, &args[0].toObject());
    RootedValue vInit(cx), vMax(cx), vShared(cx);
    if (!JS_GetProperty(cx, desc, "initial", &vInit)) return false;
    uint32_t initial = 0;
    if (!JS::ToUint32(cx, vInit, &initial)) return false;

    bool hasMax = false;
    uint32_t maximum = 65536;
    if (!JS_GetProperty(cx, desc, "maximum", &vMax)) return false;
    if (!vMax.isUndefined()) {
        hasMax = true;
        if (!JS::ToUint32(cx, vMax, &maximum)) return false;
    }

    bool shared = false;
    if (!JS_GetProperty(cx, desc, "shared", &vShared)) return false;
    if (!vShared.isUndefined()) {
        shared = JS::ToBoolean(vShared);
    }

    if (shared && !hasMax) {
        JS_ReportErrorASCII(cx, "Shared WebAssembly.Memory requires maximum");
        return false;
    }

    // SpiderMonkey 52 TypedArray/SharedArrayBuffer lengths are capped at INT32_MAX (32767 * 64KB pages).
    // Throw RangeError so apps with a memory-size fallback ladder cleanly step down below 2 GiB.
    if (initial > 32767) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_SHARED_ARRAY_BAD_LENGTH);
        return false;
    }

    wasm_memorytype_t* mty = nullptr;
    wasmtime_error_t* err = wasmtime_memorytype_new(
        initial, hasMax, maximum, false /* is_64 */, shared, 16 /* 64KB pages */, &mty);
    if (err) {
        ThrowRuntimeError(cx, TakeWasmtimeError(err));
        return false;
    }

    JSObject* memObj = nullptr;
    if (shared) {
        wasmtime_sharedmemory_t* shmem = nullptr;
        err = wasmtime_sharedmemory_new(GetGlobalEngine(), mty, &shmem);
        wasm_memorytype_delete(mty);
        if (err) {
            ThrowRuntimeError(cx, TakeWasmtimeError(err));
            return false;
        }
        memObj = WrapSharedMemory(cx, shmem, nullptr, maximum);
    } else {
        wasmtime_memory_t mem;
        err = wasmtime_memory_new(GetStoreCtx(), mty, &mem);
        wasm_memorytype_delete(mty);
        if (err) {
            ThrowRuntimeError(cx, TakeWasmtimeError(err));
            return false;
        }
        memObj = WrapMemory(cx, mem, maximum);
    }

    if (!memObj) return false;
    args.rval().setObject(*memObj);
    return true;
}

static bool
WasmtimeMemory_bufferGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.thisv().isObject()) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Memory receiver");
        return false;
    }
    RootedObject self(cx, CheckedUnwrap(&args.thisv().toObject()));
    auto* data = UnwrapWasmtimeMemory(self);
    if (!data) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Memory receiver");
        return false;
    }

    if (data->isShared) {
        size_t curBytes = wasmtime_sharedmemory_data_size(data->shmem);
        if (data->sharedRawBuf && data->sharedRawBuf->byteLength() < curBytes) {
            (void)data->sharedRawBuf->growTo(static_cast<uint32_t>(curBytes));
        }
        Value cached = JS_GetReservedSlot(self, 1);
        args.rval().set(cached);
        return true;
    }

    wasmtime_context_t* sctx = GetStoreCtx();
    uint8_t* ptr = wasmtime_memory_data(sctx, &data->mem);
    size_t byteLen = wasmtime_memory_data_size(sctx, &data->mem);

    Value cached = JS_GetReservedSlot(self, 1);
    if (cached.isObject()) {
        RootedObject ab(cx, &cached.toObject());
        if (JS_IsArrayBufferObject(ab) && JS_GetArrayBufferByteLength(ab) == byteLen) {
            JS::AutoCheckCannotGC nogc;
            bool isShared = false;
            if (JS_GetArrayBufferData(ab, &isShared, nogc) == ptr) {
                args.rval().setObject(*ab);
                return true;
            }
        }
        JS_DetachArrayBuffer(cx, ab);
    }

    RootedObject newBuf(cx, JS_NewArrayBufferWithExternalContents(cx, byteLen, ptr));
    if (!newBuf) return false;
    JS_SetReservedSlot(self, 1, ObjectValue(*newBuf));
    args.rval().setObject(*newBuf);
    return true;
}

static bool
WasmtimeMemory_grow(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.thisv().isObject() || args.length() < 1) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Memory.grow call");
        return false;
    }
    RootedObject self(cx, CheckedUnwrap(&args.thisv().toObject()));
    auto* data = UnwrapWasmtimeMemory(self);
    if (!data) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Memory receiver");
        return false;
    }
    uint32_t delta = 0;
    if (!JS::ToUint32(cx, args[0], &delta)) return false;

    uint64_t prev = 0;
    wasmtime_error_t* err = nullptr;
    if (data->isShared) {
        size_t curPages = wasmtime_sharedmemory_size(data->shmem);
        if (curPages + delta > 32767) {
            JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW, "memory");
            return false;
        }
        err = wasmtime_sharedmemory_grow(data->shmem, delta, &prev);
        if (!err && data->sharedRawBuf) {
            (void)data->sharedRawBuf->growTo(static_cast<uint32_t>(wasmtime_sharedmemory_data_size(data->shmem)));
        }
    } else {
        size_t curPages = wasmtime_memory_size(GetStoreCtx(), &data->mem);
        if (curPages + delta > 32767) {
            JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW, "memory");
            return false;
        }
        err = wasmtime_memory_grow(GetStoreCtx(), &data->mem, delta, &prev);
        if (!err) {
            Value cached = JS_GetReservedSlot(self, 1);
            if (cached.isObject()) {
                RootedObject ab(cx, &cached.toObject());
                JS_DetachArrayBuffer(cx, ab);
            }
            JS_SetReservedSlot(self, 1, UndefinedValue());
        }
    }

    if (err) {
        std::string msg = TakeWasmtimeError(err);
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW, "memory");
        return false;
    }
    args.rval().setNumber(static_cast<double>(prev));
    return true;
}

// ============================================================================
// Table Object
// ============================================================================

struct WasmtimeTableData {
    wasmtime_table_t table = {};
    bool isFuncRef = true;
};

static void
WasmtimeTable_finalize(FreeOp* fop, JSObject* obj)
{
    auto* data = static_cast<WasmtimeTableData*>(JS_GetReservedSlot(obj, 0).toPrivate());
    delete data;
}

static const ClassOps WasmtimeTableClassOps = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    WasmtimeTable_finalize
};

static const Class WasmtimeTableClass = {
    "Table",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
    &WasmtimeTableClassOps
};

static WasmtimeTableData*
UnwrapWasmtimeTable(JSObject* obj)
{
    if (!obj) return nullptr;
    obj = CheckedUnwrap(obj);
    if (!obj || JS_GetClass(obj) != Jsvalify(&WasmtimeTableClass)) return nullptr;
    return static_cast<WasmtimeTableData*>(JS_GetReservedSlot(obj, 0).toPrivate());
}

static JSObject*
WrapTable(JSContext* cx, const wasmtime_table_t& table, bool isFuncRef)
{
    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmTable).toObject());
    RootedObject obj(cx, JS_NewObjectWithGivenProto(cx, Jsvalify(&WasmtimeTableClass), proto));
    if (!obj) return nullptr;
    auto* data = new WasmtimeTableData{ table, isFuncRef };
    JS_SetReservedSlot(obj, 0, PrivateValue(data));
    return obj;
}

static bool
WasmtimeTable_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.isConstructing() || args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Table requires a descriptor object");
        return false;
    }
    RootedObject desc(cx, &args[0].toObject());
    RootedValue vInit(cx), vMax(cx);
    if (!JS_GetProperty(cx, desc, "initial", &vInit)) return false;
    uint32_t initial = 0;
    if (!JS::ToUint32(cx, vInit, &initial)) return false;

    uint32_t maximum = wasm_limits_max_default;
    if (!JS_GetProperty(cx, desc, "maximum", &vMax)) return false;
    if (!vMax.isUndefined()) {
        if (!JS::ToUint32(cx, vMax, &maximum)) return false;
    }

    wasm_limits_t limits = { initial, maximum };
    wasm_valtype_t* elemTy = wasm_valtype_new(WASM_FUNCREF);
    wasm_tabletype_t* tty = wasm_tabletype_new(elemTy, &limits);
    wasmtime_val_t initVal;
    memset(&initVal, 0, sizeof(initVal));
    initVal.kind = WASMTIME_FUNCREF;

    wasmtime_table_t table;
    wasmtime_error_t* err = wasmtime_table_new(GetStoreCtx(), tty, &initVal, &table);
    wasm_tabletype_delete(tty);
    if (err) {
        ThrowRuntimeError(cx, TakeWasmtimeError(err));
        return false;
    }

    JSObject* obj = WrapTable(cx, table, true);
    if (!obj) return false;
    args.rval().setObject(*obj);
    return true;
}

static bool
WasmtimeTable_lengthGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    auto* data = args.thisv().isObject() ? UnwrapWasmtimeTable(&args.thisv().toObject()) : nullptr;
    if (!data) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Table receiver");
        return false;
    }
    uint64_t sz = wasmtime_table_size(GetStoreCtx(), &data->table);
    args.rval().setNumber(static_cast<double>(sz));
    return true;
}

static bool
WasmtimeTable_get(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    auto* data = args.thisv().isObject() ? UnwrapWasmtimeTable(&args.thisv().toObject()) : nullptr;
    if (!data || args.length() < 1) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Table.get call");
        return false;
    }
    uint32_t idx = 0;
    if (!JS::ToUint32(cx, args[0], &idx)) return false;
    wasmtime_val_t val;
    if (!wasmtime_table_get(GetStoreCtx(), &data->table, idx, &val)) {
        JS_ReportErrorASCII(cx, "WebAssembly.Table.get index out of bounds");
        return false;
    }
    RootedValue rval(cx);
    bool ok = WasmValToJS(cx, val, &rval);
    wasmtime_val_unroot(&val);
    if (!ok) return false;
    args.rval().set(rval);
    return true;
}

static bool
WasmtimeTable_set(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    auto* data = args.thisv().isObject() ? UnwrapWasmtimeTable(&args.thisv().toObject()) : nullptr;
    if (!data || args.length() < 1) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Table.set call");
        return false;
    }
    uint32_t idx = 0;
    if (!JS::ToUint32(cx, args[0], &idx)) return false;
    wasmtime_val_t val;
    memset(&val, 0, sizeof(val));
    val.kind = WASMTIME_FUNCREF;
    if (args.length() >= 2 && args[1].isObject()) {
        wasmtime_func_t f;
        if (IsExportedWasmtimeFunc(&args[1].toObject(), &f)) {
            val.of.funcref = f;
        }
    }
    wasmtime_error_t* err = wasmtime_table_set(GetStoreCtx(), &data->table, idx, &val);
    if (err) {
        ThrowRuntimeError(cx, TakeWasmtimeError(err));
        return false;
    }
    args.rval().setUndefined();
    return true;
}

static bool
WasmtimeTable_grow(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    auto* data = args.thisv().isObject() ? UnwrapWasmtimeTable(&args.thisv().toObject()) : nullptr;
    if (!data || args.length() < 1) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Table.grow call");
        return false;
    }
    uint32_t delta = 0;
    if (!JS::ToUint32(cx, args[0], &delta)) return false;
    wasmtime_val_t initVal;
    memset(&initVal, 0, sizeof(initVal));
    initVal.kind = WASMTIME_FUNCREF;
    uint64_t prev = 0;
    wasmtime_error_t* err = wasmtime_table_grow(GetStoreCtx(), &data->table, delta, &initVal, &prev);
    if (err) {
        TakeWasmtimeError(err);
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW, "table");
        return false;
    }
    args.rval().setNumber(static_cast<double>(prev));
    return true;
}

// ============================================================================
// Global Object
// ============================================================================

struct WasmtimeGlobalData {
    wasmtime_global_t global = {};
    wasm_valkind_t kind = WASM_I32;
    bool isMutable = false;
};

static void
WasmtimeGlobal_finalize(FreeOp* fop, JSObject* obj)
{
    auto* data = static_cast<WasmtimeGlobalData*>(JS_GetReservedSlot(obj, 0).toPrivate());
    delete data;
}

static const ClassOps WasmtimeGlobalClassOps = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    WasmtimeGlobal_finalize
};

static const Class WasmtimeGlobalClass = {
    "Global",
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
    &WasmtimeGlobalClassOps
};

static WasmtimeGlobalData*
UnwrapWasmtimeGlobal(JSObject* obj)
{
    if (!obj) return nullptr;
    obj = CheckedUnwrap(obj);
    if (!obj || JS_GetClass(obj) != Jsvalify(&WasmtimeGlobalClass)) return nullptr;
    return static_cast<WasmtimeGlobalData*>(JS_GetReservedSlot(obj, 0).toPrivate());
}

static bool
WasmtimeGlobal_valueGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    auto* data = args.thisv().isObject() ? UnwrapWasmtimeGlobal(&args.thisv().toObject()) : nullptr;
    if (!data) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Global receiver");
        return false;
    }
    wasmtime_val_t val;
    wasmtime_global_get(GetStoreCtx(), &data->global, &val);
    RootedValue rval(cx);
    bool ok = WasmValToJS(cx, val, &rval);
    wasmtime_val_unroot(&val);
    if (!ok) return false;
    args.rval().set(rval);
    return true;
}

static bool
WasmtimeGlobal_valueSetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    auto* data = args.thisv().isObject() ? UnwrapWasmtimeGlobal(&args.thisv().toObject()) : nullptr;
    if (!data || !data->isMutable || args.length() < 1) {
        JS_ReportErrorASCII(cx, "Cannot set immutable WebAssembly.Global");
        return false;
    }
    wasmtime_val_t val;
    if (!JSToWasmVal(cx, args[0], data->kind, val)) return false;
    wasmtime_error_t* err = wasmtime_global_set(GetStoreCtx(), &data->global, &val);
    if (err) {
        ThrowRuntimeError(cx, TakeWasmtimeError(err));
        return false;
    }
    args.rval().setUndefined();
    return true;
}

static JSObject*
WrapGlobal(JSContext* cx, const wasmtime_global_t& global, wasm_valkind_t kind, bool isMutable)
{
    RootedObject obj(cx, JS_NewObject(cx, Jsvalify(&WasmtimeGlobalClass)));
    if (!obj) return nullptr;
    auto* data = new WasmtimeGlobalData{ global, kind, isMutable };
    JS_SetReservedSlot(obj, 0, PrivateValue(data));
    JS_DefineProperty(cx, obj, "value", UndefinedHandleValue,
                      JSPROP_ENUMERATE | JSPROP_SHARED,
                      WasmtimeGlobal_valueGetter,
                      isMutable ? WasmtimeGlobal_valueSetter : nullptr);
    return obj;
}

static bool
WasmtimeGlobal_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.isConstructing() || args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Global requires a descriptor");
        return false;
    }
    RootedObject desc(cx, &args[0].toObject());
    RootedValue vType(cx), vMut(cx);
    if (!JS_GetProperty(cx, desc, "value", &vType)) return false;
    if (!JS_GetProperty(cx, desc, "mutable", &vMut)) return false;
    bool isMut = !vMut.isUndefined() && JS::ToBoolean(vMut);

    RootedString sType(cx, JS::ToString(cx, vType));
    if (!sType) return false;
    JSAutoByteString typeStr(cx, sType);
    wasm_valkind_t kind = WASM_I32;
    if (strcmp(typeStr.ptr(), "i32") == 0) kind = WASM_I32;
    else if (strcmp(typeStr.ptr(), "i64") == 0) kind = WASM_I64;
    else if (strcmp(typeStr.ptr(), "f32") == 0) kind = WASM_F32;
    else if (strcmp(typeStr.ptr(), "f64") == 0) kind = WASM_F64;
    else if (strcmp(typeStr.ptr(), "anyfunc") == 0 || strcmp(typeStr.ptr(), "funcref") == 0) kind = WASM_FUNCREF;

    wasmtime_val_t initVal;
    RootedValue jsInit(cx, args.length() >= 2 ? args[1] : Int32Value(0));
    if (!JSToWasmVal(cx, jsInit, kind, initVal)) return false;

    wasm_valtype_t* vt = wasm_valtype_new(kind);
    wasm_globaltype_t* gt = wasm_globaltype_new(vt, isMut ? WASM_VAR : WASM_CONST);
    wasmtime_global_t global;
    wasmtime_error_t* err = wasmtime_global_new(GetStoreCtx(), gt, &initVal, &global);
    wasm_globaltype_delete(gt);
    if (err) {
        ThrowRuntimeError(cx, TakeWasmtimeError(err));
        return false;
    }
    JSObject* obj = WrapGlobal(cx, global, kind, isMut);
    if (!obj) return false;
    args.rval().setObject(*obj);
    return true;
}

// ============================================================================
// Exported Wasm Functions & Host Callbacks
// ============================================================================

struct WasmtimeExportedFuncData {
    wasmtime_func_t func;
    std::vector<wasm_valkind_t> params;
    std::vector<wasm_valkind_t> results;
};

static bool
ExportedFuncCall(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());
    auto* fd = static_cast<WasmtimeExportedFuncData*>(
        js::GetFunctionNativeReserved(callee, 0).toPrivate());
    if (!fd) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly function");
        return false;
    }

    std::vector<wasmtime_val_t> wargs(fd->params.size());
    for (size_t i = 0; i < fd->params.size(); ++i) {
        RootedValue a(cx, i < args.length() ? args[i] : UndefinedValue());
        if (!JSToWasmVal(cx, a, fd->params[i], wargs[i])) {
            return false;
        }
    }

    std::vector<wasmtime_val_t> wres(fd->results.size());
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = nullptr;
    {
        AutoActiveCx active(cx);
        err = wasmtime_func_call(
            GetStoreCtx(), &fd->func,
            wargs.empty() ? nullptr : wargs.data(), wargs.size(),
            wres.empty() ? nullptr : wres.data(), wres.size(),
            &trap);
    }

    if (cx->isExceptionPending()) {
        if (trap) wasm_trap_delete(trap);
        if (err) wasmtime_error_delete(err);
        return false;
    }
    if (err) {
        ThrowRuntimeError(cx, TakeWasmtimeError(err));
        return false;
    }
    if (trap) {
        ThrowRuntimeError(cx, TakeTrapMessage(trap));
        return false;
    }

    if (wres.empty()) {
        args.rval().setUndefined();
        return true;
    }
    if (wres.size() == 1) {
        RootedValue rval(cx);
        bool ok = WasmValToJS(cx, wres[0], &rval);
        wasmtime_val_unroot(&wres[0]);
        if (!ok) return false;
        args.rval().set(rval);
        return true;
    }
    RootedObject arr(cx, JS_NewArrayObject(cx, wres.size()));
    if (!arr) return false;
    for (size_t i = 0; i < wres.size(); ++i) {
        RootedValue rv(cx);
        bool ok = WasmValToJS(cx, wres[i], &rv);
        wasmtime_val_unroot(&wres[i]);
        if (!ok || !JS_DefineElement(cx, arr, i, rv, JSPROP_ENUMERATE)) return false;
    }
    args.rval().setObject(*arr);
    return true;
}

static bool
IsExportedWasmtimeFunc(JSObject* obj, wasmtime_func_t* outFunc)
{
    if (!obj) return false;
    obj = CheckedUnwrap(obj);
    if (!obj || !obj->is<JSFunction>()) return false;
    JSFunction* fn = &obj->as<JSFunction>();
    if (!fn->isNative() || fn->native() != ExportedFuncCall) return false;
    auto* fd = static_cast<WasmtimeExportedFuncData*>(
        js::GetFunctionNativeReserved(obj, 0).toPrivate());
    if (!fd) return false;
    if (outFunc) *outFunc = fd->func;
    return true;
}

static JSObject*
CreateExportedFunc(JSContext* cx, const wasmtime_func_t& func, HandleObject keepAlive)
{
    wasm_functype_t* fty = wasmtime_func_type(GetStoreCtx(), &func);
    const wasm_valtype_vec_t* p = wasm_functype_params(fty);
    const wasm_valtype_vec_t* r = wasm_functype_results(fty);

    auto* fd = new WasmtimeExportedFuncData();
    fd->func = func;
    fd->params.reserve(p->size);
    for (size_t i = 0; i < p->size; ++i) fd->params.push_back(wasm_valtype_kind(p->data[i]));
    fd->results.reserve(r->size);
    for (size_t i = 0; i < r->size; ++i) fd->results.push_back(wasm_valtype_kind(r->data[i]));
    wasm_functype_delete(fty);

    RootedFunction fn(cx, js::NewFunctionWithReserved(cx, ExportedFuncCall, fd->params.size(), 0, nullptr));
    if (!fn) {
        delete fd;
        return nullptr;
    }
    js::SetFunctionNativeReserved(fn, 0, PrivateValue(fd));
    if (keepAlive) {
        js::SetFunctionNativeReserved(fn, 1, ObjectValue(*keepAlive));
    }
    return fn;
}

// Host JS function callback invoked from Wasmtime
struct HostFuncEnv {
    JS::PersistentRooted<JSObject*> jsCallable;
    std::vector<wasm_valkind_t> results;

    HostFuncEnv(JSContext* cx, HandleObject fn, const std::vector<wasm_valkind_t>& res)
      : jsCallable(cx, fn), results(res) {}
};

static void
HostFuncEnv_finalize(void* env)
{
    delete static_cast<HostFuncEnv*>(env);
}

static wasm_trap_t*
HostFuncCallback(void* env,
                 wasmtime_caller_t* caller,
                 const wasmtime_val_t* args,
                 size_t nargs,
                 wasmtime_val_t* results,
                 size_t nresults)
{
    auto* hf = static_cast<HostFuncEnv*>(env);
    JSContext* cx = GetThreadStore().activeCx;
    if (!cx || !hf || !hf->jsCallable) {
        static const char* msg = "No active JSContext for host function";
        return wasmtime_trap_new(msg, strlen(msg));
    }

    JS::AutoValueVector jsArgs(cx);
    if (!jsArgs.resize(nargs)) {
        static const char* msg = "OOM in host function call";
        return wasmtime_trap_new(msg, strlen(msg));
    }
    for (size_t i = 0; i < nargs; ++i) {
        RootedValue v(cx);
        if (!WasmValToJS(cx, args[i], &v)) {
            static const char* msg = "JS conversion error";
            return wasmtime_trap_new(msg, strlen(msg));
        }
        jsArgs[i].set(v);
    }

    RootedValue calleeVal(cx, ObjectValue(*hf->jsCallable));
    RootedValue rval(cx);
    if (!JS::Call(cx, UndefinedHandleValue, calleeVal, jsArgs, &rval)) {
        static const char* msg = "JS exception";
        return wasmtime_trap_new(msg, strlen(msg));
    }

    if (nresults == 1) {
        if (!JSToWasmVal(cx, rval, hf->results[0], results[0])) {
            static const char* msg = "JS return conversion error";
            return wasmtime_trap_new(msg, strlen(msg));
        }
    } else if (nresults > 1) {
        if (!rval.isObject()) {
            static const char* msg = "Multi-value host function must return iterable";
            return wasmtime_trap_new(msg, strlen(msg));
        }
        RootedObject arr(cx, &rval.toObject());
        for (size_t i = 0; i < nresults; ++i) {
            RootedValue elem(cx);
            if (!JS_GetElement(cx, arr, i, &elem) ||
                !JSToWasmVal(cx, elem, hf->results[i], results[i])) {
                static const char* msg = "Multi-value conversion error";
                return wasmtime_trap_new(msg, strlen(msg));
            }
        }
    }
    return nullptr;
}

// ============================================================================
// Instance Object
// ============================================================================

struct WasmtimeInstanceData {
    wasmtime_instance_t instance = {};
};

static void
WasmtimeInstance_finalize(FreeOp* fop, JSObject* obj)
{
    auto* data = static_cast<WasmtimeInstanceData*>(JS_GetReservedSlot(obj, 0).toPrivate());
    delete data;
}

static const ClassOps WasmtimeInstanceClassOps = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    WasmtimeInstance_finalize
};

static const Class WasmtimeInstanceClass = {
    "Instance",
    JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_FOREGROUND_FINALIZE,
    &WasmtimeInstanceClassOps
};

static bool
WasmtimeInstance_exportsGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.thisv().isObject()) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Instance receiver");
        return false;
    }
    RootedObject self(cx, CheckedUnwrap(&args.thisv().toObject()));
    if (!self || JS_GetClass(self) != Jsvalify(&WasmtimeInstanceClass)) {
        JS_ReportErrorASCII(cx, "Invalid WebAssembly.Instance receiver");
        return false;
    }
    args.rval().set(JS_GetReservedSlot(self, 1));
    return true;
}

static JSObject*
InstantiateModule(JSContext* cx, wasmtime_module_t* mod, HandleObject importObj)
{
    wasmtime_context_t* sctx = GetStoreCtx();

    wasm_importtype_vec_t importTypes;
    wasmtime_module_imports(mod, &importTypes);

    std::vector<wasmtime_extern_t> externs(importTypes.size);
    std::vector<JSObject*> importedMemories;
    std::vector<JSObject*> importedTables;
    std::vector<JSObject*> importedGlobals;

    for (size_t i = 0; i < importTypes.size; ++i) {
        const wasm_importtype_t* it = importTypes.data[i];
        const wasm_name_t* mname = wasm_importtype_module(it);
        const wasm_name_t* fname = wasm_importtype_name(it);
        const wasm_externtype_t* et = wasm_importtype_type(it);

        std::string modStr(mname->data, mname->size);
        std::string fieldStr(fname->data, fname->size);

        if (!importObj) {
            wasm_importtype_vec_delete(&importTypes);
            ThrowLinkError(cx, "Missing import object for " + modStr + "." + fieldStr);
            return nullptr;
        }

        RootedValue modVal(cx);
        if (!JS_GetProperty(cx, importObj, modStr.c_str(), &modVal) || !modVal.isObject()) {
            wasm_importtype_vec_delete(&importTypes);
            ThrowLinkError(cx, "Missing import module: " + modStr);
            return nullptr;
        }
        RootedObject modObj(cx, &modVal.toObject());
        RootedValue fieldVal(cx);
        if (!JS_GetProperty(cx, modObj, fieldStr.c_str(), &fieldVal)) {
            wasm_importtype_vec_delete(&importTypes);
            return nullptr;
        }

        wasm_externkind_t kind = wasm_externtype_kind(et);
        if (kind == WASM_EXTERN_FUNC) {
            if (!fieldVal.isObject() || !JS::IsCallable(&fieldVal.toObject())) {
                wasm_importtype_vec_delete(&importTypes);
                ThrowLinkError(cx, "Import " + modStr + "." + fieldStr + " must be a function");
                return nullptr;
            }
            RootedObject fnObj(cx, &fieldVal.toObject());
            wasmtime_func_t existing;
            if (IsExportedWasmtimeFunc(fnObj, &existing)) {
                externs[i].kind = WASMTIME_EXTERN_FUNC;
                externs[i].of.func = existing;
            } else {
                const wasm_functype_t* fty = wasm_externtype_as_functype_const(et);
                const wasm_valtype_vec_t* r = wasm_functype_results(fty);
                std::vector<wasm_valkind_t> resKinds;
                resKinds.reserve(r->size);
                for (size_t k = 0; k < r->size; ++k) resKinds.push_back(wasm_valtype_kind(r->data[k]));
                auto* env = new HostFuncEnv(cx, fnObj, resKinds);
                wasmtime_func_t hostFunc;
                wasmtime_func_new(sctx, fty, HostFuncCallback, env, HostFuncEnv_finalize, &hostFunc);
                externs[i].kind = WASMTIME_EXTERN_FUNC;
                externs[i].of.func = hostFunc;
            }
        } else if (kind == WASM_EXTERN_MEMORY) {
            auto* md = fieldVal.isObject() ? UnwrapWasmtimeMemory(&fieldVal.toObject()) : nullptr;
            if (!md) {
                wasm_importtype_vec_delete(&importTypes);
                ThrowLinkError(cx, "Import " + modStr + "." + fieldStr + " must be a WebAssembly.Memory");
                return nullptr;
            }
            if (md->isShared) {
                externs[i].kind = WASMTIME_EXTERN_SHAREDMEMORY;
                externs[i].of.sharedmemory = md->shmem;
            } else {
                externs[i].kind = WASMTIME_EXTERN_MEMORY;
                externs[i].of.memory = md->mem;
            }
            importedMemories.push_back(&fieldVal.toObject());
        } else if (kind == WASM_EXTERN_TABLE) {
            auto* td = fieldVal.isObject() ? UnwrapWasmtimeTable(&fieldVal.toObject()) : nullptr;
            if (!td) {
                wasm_importtype_vec_delete(&importTypes);
                ThrowLinkError(cx, "Import " + modStr + "." + fieldStr + " must be a WebAssembly.Table");
                return nullptr;
            }
            externs[i].kind = WASMTIME_EXTERN_TABLE;
            externs[i].of.table = td->table;
            importedTables.push_back(&fieldVal.toObject());
        } else if (kind == WASM_EXTERN_GLOBAL) {
            auto* gd = fieldVal.isObject() ? UnwrapWasmtimeGlobal(&fieldVal.toObject()) : nullptr;
            if (gd) {
                externs[i].kind = WASMTIME_EXTERN_GLOBAL;
                externs[i].of.global = gd->global;
                importedGlobals.push_back(&fieldVal.toObject());
            } else {
                const wasm_globaltype_t* gty = wasm_externtype_as_globaltype_const(et);
                wasm_valkind_t vk = wasm_valtype_kind(wasm_globaltype_content(gty));
                wasmtime_val_t gv;
                if (!JSToWasmVal(cx, fieldVal, vk, gv)) {
                    wasm_importtype_vec_delete(&importTypes);
                    return nullptr;
                }
                wasmtime_global_t rawG;
                wasmtime_error_t* gerr = wasmtime_global_new(sctx, gty, &gv, &rawG);
                if (gerr) {
                    wasm_importtype_vec_delete(&importTypes);
                    ThrowLinkError(cx, TakeWasmtimeError(gerr));
                    return nullptr;
                }
                externs[i].kind = WASMTIME_EXTERN_GLOBAL;
                externs[i].of.global = rawG;
            }
        }
    }
    wasm_importtype_vec_delete(&importTypes);

    wasmtime_instance_t rawInst;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = nullptr;
    {
        AutoActiveCx active(cx);
        err = wasmtime_instance_new(
            sctx, mod,
            externs.empty() ? nullptr : externs.data(), externs.size(),
            &rawInst, &trap);
    }

    if (cx->isExceptionPending()) {
        if (trap) wasm_trap_delete(trap);
        if (err) wasmtime_error_delete(err);
        return nullptr;
    }
    if (err) {
        ThrowLinkError(cx, TakeWasmtimeError(err));
        return nullptr;
    }
    if (trap) {
        ThrowRuntimeError(cx, TakeTrapMessage(trap));
        return nullptr;
    }

    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmInstance).toObject());
    RootedObject instObj(cx, JS_NewObjectWithGivenProto(cx, Jsvalify(&WasmtimeInstanceClass), proto));
    if (!instObj) return nullptr;
    JS_SetReservedSlot(instObj, 0, PrivateValue(new WasmtimeInstanceData{ rawInst }));

    RootedObject exportsObj(cx, JS_NewPlainObject(cx));
    if (!exportsObj) return nullptr;

    size_t idx = 0;
    char* ename = nullptr;
    size_t enameLen = 0;
    wasmtime_extern_t item;
    while (wasmtime_instance_export_nth(sctx, &rawInst, idx++, &ename, &enameLen, &item)) {
        std::string nameStr(ename, enameLen);
        RootedValue exportVal(cx);

        if (item.kind == WASMTIME_EXTERN_FUNC) {
            JSObject* fn = CreateExportedFunc(cx, item.of.func, instObj);
            if (!fn) { wasmtime_extern_delete(&item); return nullptr; }
            exportVal.setObject(*fn);
        } else if (item.kind == WASMTIME_EXTERN_MEMORY) {
            // Reuse imported memory object if it matches, else wrap new.
            JSObject* memObj = nullptr;
            for (JSObject* im : importedMemories) {
                auto* md = UnwrapWasmtimeMemory(im);
                if (md && !md->isShared &&
                    md->mem.store_id == item.of.memory.store_id &&
                    md->mem.__private1 == item.of.memory.__private1 &&
                    md->mem.__private2 == item.of.memory.__private2) {
                    memObj = im;
                    break;
                }
            }
            if (!memObj) {
                wasm_memorytype_t* mty = wasmtime_memory_type(sctx, &item.of.memory);
                uint64_t maxP = 65536;
                wasmtime_memorytype_maximum(mty, &maxP);
                wasm_memorytype_delete(mty);
                memObj = WrapMemory(cx, item.of.memory, static_cast<uint32_t>(maxP));
            }
            if (!memObj) { wasmtime_extern_delete(&item); return nullptr; }
            exportVal.setObject(*memObj);
        } else if (item.kind == WASMTIME_EXTERN_SHAREDMEMORY) {
            JSObject* memObj = nullptr;
            uint8_t* ptr = wasmtime_sharedmemory_data(item.of.sharedmemory);
            for (JSObject* im : importedMemories) {
                auto* md = UnwrapWasmtimeMemory(im);
                if (md && md->isShared && wasmtime_sharedmemory_data(md->shmem) == ptr) {
                    memObj = im;
                    break;
                }
            }
            if (!memObj) {
                wasm_memorytype_t* mty = wasmtime_sharedmemory_type(item.of.sharedmemory);
                uint64_t maxP = 65536;
                wasmtime_memorytype_maximum(mty, &maxP);
                wasm_memorytype_delete(mty);
                wasmtime_sharedmemory_t* cloned = wasmtime_sharedmemory_clone(item.of.sharedmemory);
                memObj = WrapSharedMemory(cx, cloned, nullptr, static_cast<uint32_t>(maxP));
            }
            if (!memObj) { wasmtime_extern_delete(&item); return nullptr; }
            exportVal.setObject(*memObj);
        } else if (item.kind == WASMTIME_EXTERN_TABLE) {
            JSObject* tabObj = nullptr;
            for (JSObject* it : importedTables) {
                auto* td = UnwrapWasmtimeTable(it);
                if (td && td->table.store_id == item.of.table.store_id &&
                    td->table.__private1 == item.of.table.__private1 &&
                    td->table.__private2 == item.of.table.__private2) {
                    tabObj = it;
                    break;
                }
            }
            if (!tabObj) {
                tabObj = WrapTable(cx, item.of.table, true);
            }
            if (!tabObj) { wasmtime_extern_delete(&item); return nullptr; }
            exportVal.setObject(*tabObj);
        } else if (item.kind == WASMTIME_EXTERN_GLOBAL) {
            wasm_globaltype_t* gty = wasmtime_global_type(sctx, &item.of.global);
            wasm_valkind_t vk = wasm_valtype_kind(wasm_globaltype_content(gty));
            bool isMut = (wasm_globaltype_mutability(gty) == WASM_VAR);
            wasm_globaltype_delete(gty);
            JSObject* gObj = WrapGlobal(cx, item.of.global, vk, isMut);
            if (!gObj) { wasmtime_extern_delete(&item); return nullptr; }
            exportVal.setObject(*gObj);
        }

        wasmtime_extern_delete(&item);
        JS_DefineProperty(cx, exportsObj, nameStr.c_str(), exportVal, JSPROP_ENUMERATE | JSPROP_READONLY);
    }

    JS_FreezeObject(cx, exportsObj);
    JS_SetReservedSlot(instObj, 1, ObjectValue(*exportsObj));
    return instObj;
}

static bool
WasmtimeInstance_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.isConstructing() || args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.Instance requires a Module");
        return false;
    }
    auto* md = UnwrapWasmtimeModule(&args[0].toObject());
    if (!md) {
        JS_ReportErrorASCII(cx, "First argument must be a WebAssembly.Module");
        return false;
    }
    RootedObject importObj(cx);
    if (args.length() >= 2 && !args[1].isUndefined()) {
        if (!args[1].isObject()) {
            JS_ReportErrorASCII(cx, "Second argument must be an object");
            return false;
        }
        importObj = &args[1].toObject();
    }
    JSObject* inst = InstantiateModule(cx, md->module, importObj);
    if (!inst) return false;
    args.rval().setObject(*inst);
    return true;
}

// ============================================================================
// Top-level WebAssembly static methods
// ============================================================================

static bool
NopFn(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return true;
}

static PromiseObject*
CreatePromise(JSContext* cx)
{
    RootedFunction nop(cx, NewNativeFunction(cx, NopFn, 0, nullptr));
    if (!nop) return nullptr;
    return PromiseObject::create(cx, nop);
}

static bool
RejectPendingToPromise(JSContext* cx, Handle<PromiseObject*> promise, CallArgs& args)
{
    RootedValue exc(cx);
    if (!JS_GetPendingException(cx, &exc)) return false;
    JS_ClearPendingException(cx);
    if (!PromiseObject::reject(cx, promise, exc)) return false;
    args.rval().setObject(*promise);
    return true;
}

static bool
Wasmtime_validate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1) {
        JS_ReportErrorASCII(cx, "WebAssembly.validate requires a BufferSource");
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ExtractBytes(cx, args[0], bytes)) return false;
    wasmtime_error_t* err = wasmtime_module_validate(GetGlobalEngine(), bytes.data(), bytes.size());
    bool ok = (err == nullptr);
    if (err) wasmtime_error_delete(err);
    args.rval().setBoolean(ok);
    return true;
}

static bool
Wasmtime_compile(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Rooted<PromiseObject*> promise(cx, CreatePromise(cx));
    if (!promise) return false;

    if (args.length() < 1) {
        JS_ReportErrorASCII(cx, "WebAssembly.compile requires a BufferSource");
        return RejectPendingToPromise(cx, promise, args);
    }
    std::vector<uint8_t> bytes;
    if (!ExtractBytes(cx, args[0], bytes)) {
        return RejectPendingToPromise(cx, promise, args);
    }
    wasmtime_module_t* mod = nullptr;
    wasmtime_error_t* err = wasmtime_module_new(GetGlobalEngine(), bytes.data(), bytes.size(), &mod);
    if (err) {
        ThrowCompileError(cx, TakeWasmtimeError(err));
        return RejectPendingToPromise(cx, promise, args);
    }
    RootedObject modObj(cx, WrapWasmtimeModule(cx, mod, std::move(bytes)));
    if (!modObj) return false;
    RootedValue rv(cx, ObjectValue(*modObj));
    if (!PromiseObject::resolve(cx, promise, rv)) return false;
    args.rval().setObject(*promise);
    return true;
}

static bool
Wasmtime_instantiate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Rooted<PromiseObject*> promise(cx, CreatePromise(cx));
    if (!promise) return false;

    if (args.length() < 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "WebAssembly.instantiate requires a BufferSource or Module");
        return RejectPendingToPromise(cx, promise, args);
    }

    RootedObject importObj(cx);
    if (args.length() >= 2 && !args[1].isUndefined()) {
        if (!args[1].isObject()) {
            JS_ReportErrorASCII(cx, "importObject must be an object");
            return RejectPendingToPromise(cx, promise, args);
        }
        importObj = &args[1].toObject();
    }

    auto* existingMod = UnwrapWasmtimeModule(&args[0].toObject());
    if (existingMod) {
        RootedObject instObj(cx, InstantiateModule(cx, existingMod->module, importObj));
        if (!instObj) return RejectPendingToPromise(cx, promise, args);
        RootedValue rv(cx, ObjectValue(*instObj));
        if (!PromiseObject::resolve(cx, promise, rv)) return false;
        args.rval().setObject(*promise);
        return true;
    }

    std::vector<uint8_t> bytes;
    if (!ExtractBytes(cx, args[0], bytes)) {
        return RejectPendingToPromise(cx, promise, args);
    }
    wasmtime_module_t* mod = nullptr;
    wasmtime_error_t* err = wasmtime_module_new(GetGlobalEngine(), bytes.data(), bytes.size(), &mod);
    if (err) {
        ThrowCompileError(cx, TakeWasmtimeError(err));
        return RejectPendingToPromise(cx, promise, args);
    }
    RootedObject modObj(cx, WrapWasmtimeModule(cx, mod, std::move(bytes)));
    if (!modObj) return false;

    RootedObject instObj(cx, InstantiateModule(cx, mod, importObj));
    if (!instObj) return RejectPendingToPromise(cx, promise, args);

    RootedObject res(cx, JS_NewPlainObject(cx));
    if (!res) return false;
    RootedValue mv(cx, ObjectValue(*modObj));
    RootedValue iv(cx, ObjectValue(*instObj));
    JS_DefineProperty(cx, res, "module", mv, JSPROP_ENUMERATE);
    JS_DefineProperty(cx, res, "instance", iv, JSPROP_ENUMERATE);

    RootedValue rv(cx, ObjectValue(*res));
    if (!PromiseObject::resolve(cx, promise, rv)) return false;
    args.rval().setObject(*promise);
    return true;
}

// ============================================================================
// Class initialization
// ============================================================================

static const JSFunctionSpec Wasmtime_static_methods[] = {
    JS_FN("validate", Wasmtime_validate, 1, 0),
    JS_FN("compile", Wasmtime_compile, 1, 0),
    JS_FN("instantiate", Wasmtime_instantiate, 2, 0),
    JS_FS_END
};

static const JSFunctionSpec WasmtimeModule_static_methods[] = {
    JS_FN("imports", WasmtimeModule_imports, 1, 0),
    JS_FN("exports", WasmtimeModule_exports, 1, 0),
    JS_FN("customSections", WasmtimeModule_customSections, 2, 0),
    JS_FS_END
};

static const JSPropertySpec WasmtimeMemory_props[] = {
    JS_PSG("buffer", WasmtimeMemory_bufferGetter, 0),
    JS_PS_END
};

static const JSFunctionSpec WasmtimeMemory_methods[] = {
    JS_FN("grow", WasmtimeMemory_grow, 1, 0),
    JS_FS_END
};

static const JSPropertySpec WasmtimeTable_props[] = {
    JS_PSG("length", WasmtimeTable_lengthGetter, 0),
    JS_PS_END
};

static const JSFunctionSpec WasmtimeTable_methods[] = {
    JS_FN("get", WasmtimeTable_get, 1, 0),
    JS_FN("set", WasmtimeTable_set, 2, 0),
    JS_FN("grow", WasmtimeTable_grow, 1, 0),
    JS_FS_END
};

static const JSPropertySpec WasmtimeInstance_props[] = {
    JS_PSG("exports", WasmtimeInstance_exportsGetter, 0),
    JS_PS_END
};

static bool
InitWasmCtor(JSContext* cx, HandleObject wasmObj, const char* name,
             JSNative ctorFn, unsigned nargs,
             const JSPropertySpec* props, const JSFunctionSpec* methods,
             const JSFunctionSpec* staticMethods,
             MutableHandleObject outProto)
{
    outProto.set(NewBuiltinClassInstance<PlainObject>(cx, SingletonObject));
    if (!outProto) return false;
    if (props && !JS_DefineProperties(cx, outProto, props)) return false;
    if (methods && !JS_DefineFunctions(cx, outProto, methods)) return false;

    RootedAtom atom(cx, Atomize(cx, name, strlen(name)));
    if (!atom) return false;
    RootedFunction ctor(cx, NewNativeConstructor(cx, ctorFn, nargs, atom));
    if (!ctor) return false;
    if (staticMethods && !JS_DefineFunctions(cx, ctor, staticMethods)) return false;
    if (!LinkConstructorAndPrototype(cx, ctor, outProto)) return false;

    RootedValue cv(cx, ObjectValue(*ctor));
    return JS_DefineProperty(cx, wasmObj, name, cv, JSPROP_ENUMERATE);
}

} // anonymous namespace

JSObject*
InitWasmtimeWebAssemblyClass(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();

    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto) return nullptr;

    RootedObject wasmObj(cx, NewObjectWithGivenProto(cx, &WebAssemblyClass, objProto, SingletonObject));
    if (!wasmObj) return nullptr;

    if (!JS_DefineFunctions(cx, wasmObj, Wasmtime_static_methods)) return nullptr;

    RootedObject modProto(cx), instProto(cx), memProto(cx), tabProto(cx), globProto(cx);
    if (!InitWasmCtor(cx, wasmObj, "Module", WasmtimeModule_construct, 1,
                      nullptr, nullptr, WasmtimeModule_static_methods, &modProto)) return nullptr;
    if (!InitWasmCtor(cx, wasmObj, "Instance", WasmtimeInstance_construct, 2,
                      WasmtimeInstance_props, nullptr, nullptr, &instProto)) return nullptr;
    if (!InitWasmCtor(cx, wasmObj, "Memory", WasmtimeMemory_construct, 1,
                      WasmtimeMemory_props, WasmtimeMemory_methods, nullptr, &memProto)) return nullptr;
    if (!InitWasmCtor(cx, wasmObj, "Table", WasmtimeTable_construct, 1,
                      WasmtimeTable_props, WasmtimeTable_methods, nullptr, &tabProto)) return nullptr;
    if (!InitWasmCtor(cx, wasmObj, "Global", WasmtimeGlobal_construct, 2,
                      nullptr, nullptr, nullptr, &globProto)) return nullptr;

    // Error constructors: CompileError, LinkError, RuntimeError
    RootedObject compileErrProto(cx, GlobalObject::getOrCreateCustomErrorPrototype(cx, global, JSEXN_WASMCOMPILEERROR));
    RootedObject runtimeErrProto(cx, GlobalObject::getOrCreateCustomErrorPrototype(cx, global, JSEXN_WASMRUNTIMEERROR));
    if (!compileErrProto || !runtimeErrProto) return nullptr;
    RootedValue ceCtor(cx, global->getConstructor(GetExceptionProtoKey(JSEXN_WASMCOMPILEERROR)));
    RootedValue reCtor(cx, global->getConstructor(GetExceptionProtoKey(JSEXN_WASMRUNTIMEERROR)));
    JS_DefineProperty(cx, wasmObj, "CompileError", ceCtor, JSPROP_ENUMERATE);
    JS_DefineProperty(cx, wasmObj, "RuntimeError", reCtor, JSPROP_ENUMERATE);

    // Define LinkError + compileStreaming + instantiateStreaming via small JS helper.
    static const char* kStreamingHelper =
        "(function(W) {"
        "  function LinkError(msg) { var e = new Error(msg); e.name = 'LinkError'; return e; }"
        "  LinkError.prototype = Object.create(Error.prototype);"
        "  LinkError.prototype.constructor = LinkError;"
        "  LinkError.prototype.name = 'LinkError';"
        "  W.LinkError = LinkError;"
        "  W.compileStreaming = function(src) {"
        "    return Promise.resolve(src).then(function(r) {"
        "      if (!r || typeof r.arrayBuffer !== 'function') throw new TypeError('Invalid Response');"
        "      return r.arrayBuffer();"
        "    }).then(function(buf) { return W.compile(buf); });"
        "  };"
        "  W.instantiateStreaming = function(src, imports) {"
        "    return Promise.resolve(src).then(function(r) {"
        "      if (!r || typeof r.arrayBuffer !== 'function') throw new TypeError('Invalid Response');"
        "      return r.arrayBuffer();"
        "    }).then(function(buf) { return W.instantiate(buf, imports); });"
        "  };"
        "})";
    JS::CompileOptions copts(cx);
    copts.setFileAndLine("wasmtime-streaming", 1);
    RootedValue helperFn(cx);
    if (JS::Evaluate(cx, copts, kStreamingHelper, strlen(kStreamingHelper), &helperFn) && helperFn.isObject()) {
        JS::AutoValueArray<1> hargs(cx);
        hargs[0].setObject(*wasmObj);
        RootedValue hrval(cx);
        JS_CallFunctionValue(cx, nullptr, helperFn, hargs, &hrval);
    }

    if (!JS_DefineProperty(cx, global, js_WebAssembly_str, wasmObj, JSPROP_RESOLVING))
        return nullptr;

    global->setPrototype(JSProto_WasmModule, ObjectValue(*modProto));
    global->setPrototype(JSProto_WasmInstance, ObjectValue(*instProto));
    global->setPrototype(JSProto_WasmMemory, ObjectValue(*memProto));
    global->setPrototype(JSProto_WasmTable, ObjectValue(*tabProto));
    global->setConstructor(JSProto_WebAssembly, ObjectValue(*wasmObj));
    return wasmObj;
}

// ============================================================================
// StructuredClone support for cross-Worker postMessage(Module / SharedMemory)
// ============================================================================

bool
IsWasmtimeModuleObject(JSObject* obj)
{
    return UnwrapWasmtimeModule(obj) != nullptr;
}

bool
IsWasmtimeSharedMemoryObject(JSObject* obj)
{
    auto* md = UnwrapWasmtimeMemory(obj);
    return md && md->isShared && md->shmem && md->sharedRawBuf;
}

bool
WriteWasmtimeModule(JSContext* cx, HandleObject obj, void** outClonedModule)
{
    auto* md = UnwrapWasmtimeModule(obj);
    if (!md) return false;
    *outClonedModule = wasmtime_module_clone(md->module);
    return *outClonedModule != nullptr;
}

JSObject*
ReadWasmtimeModule(JSContext* cx, void* clonedModule)
{
    if (!clonedModule) return nullptr;
    // Ensure WebAssembly class is initialized on this global/Worker before wrapping.
    RootedValue wasmVal(cx);
    if (!JS_GetProperty(cx, cx->global(), "WebAssembly", &wasmVal)) return nullptr;
    std::vector<uint8_t> empty;
    return WrapWasmtimeModule(cx, static_cast<wasmtime_module_t*>(clonedModule), std::move(empty));
}

bool
WriteWasmtimeSharedMemory(JSContext* cx, HandleObject obj,
                          void** outClonedShmem, void** outRawBuf, uint32_t* outMaxPages)
{
    auto* md = UnwrapWasmtimeMemory(obj);
    if (!md || !md->isShared || !md->shmem || !md->sharedRawBuf) return false;
    if (!md->sharedRawBuf->addReference()) return false;
    *outClonedShmem = wasmtime_sharedmemory_clone(md->shmem);
    *outRawBuf = md->sharedRawBuf;
    *outMaxPages = md->maxPages;
    return *outClonedShmem != nullptr;
}

JSObject*
ReadWasmtimeSharedMemory(JSContext* cx, void* clonedShmem, void* rawBuf, uint32_t maxPages)
{
    if (!clonedShmem || !rawBuf) return nullptr;
    RootedValue wasmVal(cx);
    if (!JS_GetProperty(cx, cx->global(), "WebAssembly", &wasmVal)) return nullptr;
    auto* rb = static_cast<SharedArrayRawBuffer*>(rawBuf);
    JSObject* obj = WrapSharedMemory(cx, static_cast<wasmtime_sharedmemory_t*>(clonedShmem), rb, maxPages);
    // WrapSharedMemory added its own ref; release the transfer ref from WriteWasmtimeSharedMemory.
    rb->dropReference();
    return obj;
}

int32_t
NotifyWasmtimeSharedMemory(void* externalOwner, uint32_t byteOffset, uint32_t count)
{
    auto* owner = static_cast<WasmtimeSharedMemoryOwner*>(externalOwner);
    if (!owner || !owner->shmem || count == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(owner->notifyMutex);
    if (owner->notifyFailed) {
        return 0;
    }
    if (!owner->notifyReady) {
        wasmtime_module_t* mod = GetNotifyHelperModule();
        if (!mod) {
            owner->notifyFailed = true;
            return 0;
        }
        owner->notifyStore = wasmtime_store_new(GetGlobalEngine(), nullptr, nullptr);
        owner->notifyCtx = wasmtime_store_context(owner->notifyStore);
        wasmtime_extern_t imp;
        imp.kind = WASMTIME_EXTERN_SHAREDMEMORY;
        imp.of.sharedmemory = wasmtime_sharedmemory_clone(owner->shmem);
        wasmtime_instance_t inst;
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_instance_new(owner->notifyCtx, mod, &imp, 1, &inst, &trap);
        wasmtime_sharedmemory_delete(imp.of.sharedmemory);
        if (err || trap) {
            if (err) wasmtime_error_delete(err);
            if (trap) wasm_trap_delete(trap);
            owner->notifyFailed = true;
            return 0;
        }
        wasmtime_extern_t exp;
        if (!wasmtime_instance_export_get(owner->notifyCtx, &inst, "n", 1, &exp) ||
            exp.kind != WASMTIME_EXTERN_FUNC) {
            owner->notifyFailed = true;
            return 0;
        }
        owner->notifyFunc = exp.of.func;
        owner->notifyReady = true;
    }

    wasmtime_val_t params[2];
    params[0].kind = WASMTIME_I32;
    params[0].of.i32 = static_cast<int32_t>(byteOffset);
    params[1].kind = WASMTIME_I32;
    params[1].of.i32 = static_cast<int32_t>(count);
    wasmtime_val_t results[1];
    results[0].kind = WASMTIME_I32;
    results[0].of.i32 = 0;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err = wasmtime_func_call(
        owner->notifyCtx, &owner->notifyFunc, params, 2, results, 1, &trap);
    if (err) {
        wasmtime_error_delete(err);
        return 0;
    }
    if (trap) {
        wasm_trap_delete(trap);
        return 0;
    }
    return results[0].of.i32;
}

} // namespace wasm
} // namespace js
