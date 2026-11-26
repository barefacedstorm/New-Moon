/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebGPU.h"
#include "WebGPUHelpers.h"

#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/dom/HTMLCanvasElementBinding.h"
#include "mozilla/dom/ImageBitmap.h"
#include "mozilla/dom/ImageBitmapBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/DataSurfaceHelpers.h"
#include "nsComponentManagerUtils.h"
#include "nsITimer.h"
#include "nsJSUtils.h"
#include "nsThreadUtils.h"

#include <atomic>
#include <string.h>
#include <vector>

using namespace mozilla::dom::webgpu_helpers;

namespace mozilla {
namespace dom {

namespace {

static JSObject*
CreateStringSet(JSContext* aCx, const nsTArray<nsCString>& aItems)
{
  JS::Rooted<JSObject*> arr(aCx, JS_NewArrayObject(aCx, aItems.Length()));
  if (!arr) return nullptr;
  for (uint32_t i = 0; i < aItems.Length(); ++i) {
    JS::Rooted<JS::Value> v(aCx);
    nsString u16 = NS_ConvertUTF8toUTF16(aItems[i]);
    if (ToJSValue(aCx, u16, &v)) {
      JS_DefineElement(aCx, arr, i, v, JSPROP_ENUMERATE);
    }
  }
  static const char* kMakeSet = "(function(a){ return new Set(a); })";
  JS::CompileOptions opts(aCx);
  JS::Rooted<JS::Value> fn(aCx);
  if (JS::Evaluate(aCx, opts, kMakeSet, strlen(kMakeSet), &fn) && fn.isObject()) {
    JS::AutoValueArray<1> args(aCx);
    args[0].setObject(*arr);
    JS::Rooted<JS::Value> res(aCx);
    if (JS_CallFunctionValue(aCx, nullptr, fn, args, &res) && res.isObject()) {
      return &res.toObject();
    }
  }
  return arr;
}

static bool
ExtractBufferSource(const ArrayBufferViewOrArrayBuffer& aData,
                    const uint8_t*& aOutPtr,
                    size_t& aOutByteLen,
                    size_t& aOutElementSize)
{
  aOutElementSize = 1;
  if (aData.IsArrayBuffer()) {
    const ArrayBuffer& ab = aData.GetAsArrayBuffer();
    ab.ComputeLengthAndData();
    aOutPtr = ab.Data();
    aOutByteLen = ab.Length();
    return aOutPtr != nullptr;
  }
  const ArrayBufferView& abv = aData.GetAsArrayBufferView();
  abv.ComputeLengthAndData();
  aOutPtr = abv.Data();
  aOutByteLen = abv.Length();
  JSObject* obj = abv.Obj();
  if (obj && JS_IsTypedArrayObject(obj)) {
    uint32_t elemCount = JS_GetTypedArrayLength(obj);
    if (elemCount > 0 && aOutByteLen >= elemCount) {
      aOutElementSize = aOutByteLen / elemCount;
    }
  }
  return aOutPtr != nullptr;
}

static bool
ParseTexelCopyTextureInfo(JSContext* aCx, JS::Handle<JS::Value> aVal, WGPUTexelCopyTextureInfo& aOut)
{
  aOut = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  if (!aVal.isObject()) return false;
  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());
  JS::Rooted<JSObject*> texObj(aCx);
  if (!GetPropObj(aCx, obj, "texture", &texObj)) return false;
  RefPtr<GPUTexture> tex;
  if (NS_FAILED(UNWRAP_OBJECT(GPUTexture, texObj, tex)) || !tex) return false;
  aOut.texture = tex->Raw();
  GetPropU32(aCx, obj, "mipLevel", aOut.mipLevel);
  JS::Rooted<JS::Value> origVal(aCx);
  if (GetPropVal(aCx, obj, "origin", &origVal)) {
    aOut.origin = ParseOrigin3D(aCx, origVal);
  }
  nsCString aspectStr;
  if (GetPropString(aCx, obj, "aspect", aspectStr)) {
    aOut.aspect = ParseTextureAspect(aspectStr);
  }
  return true;
}

static bool
ParseTexelCopyBufferLayout(JSContext* aCx, JS::Handle<JS::Value> aVal, WGPUTexelCopyBufferLayout& aOut)
{
  aOut = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
  if (!aVal.isObject()) return true;
  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());
  GetPropU64(aCx, obj, "offset", aOut.offset);
  GetPropU32(aCx, obj, "bytesPerRow", aOut.bytesPerRow);
  GetPropU32(aCx, obj, "rowsPerImage", aOut.rowsPerImage);
  return true;
}

static bool
ParseTexelCopyBufferInfo(JSContext* aCx, JS::Handle<JS::Value> aVal, WGPUTexelCopyBufferInfo& aOut)
{
  aOut = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
  if (!aVal.isObject()) return false;
  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());
  JS::Rooted<JSObject*> bufObj(aCx);
  if (!GetPropObj(aCx, obj, "buffer", &bufObj)) return false;
  RefPtr<GPUBuffer> buf;
  if (NS_FAILED(UNWRAP_OBJECT(GPUBuffer, bufObj, buf)) || !buf) return false;
  aOut.buffer = buf->Raw();
  return ParseTexelCopyBufferLayout(aCx, aVal, aOut.layout);
}

static void
OnAdapterReady(WGPURequestAdapterStatus aStatus,
               WGPUAdapter aAdapter,
               WGPUStringView aMessage,
               void* aUserdata1,
               void* aUserdata2)
{
  WGPUAdapter* out = static_cast<WGPUAdapter*>(aUserdata1);
  if (out && aStatus == WGPURequestAdapterStatus_Success) {
    *out = aAdapter;
  }
}

static void
OnDeviceReady(WGPURequestDeviceStatus aStatus,
              WGPUDevice aDevice,
              WGPUStringView aMessage,
              void* aUserdata1,
              void* aUserdata2)
{
  WGPUDevice* out = static_cast<WGPUDevice*>(aUserdata1);
  if (out && aStatus == WGPURequestDeviceStatus_Success) {
    *out = aDevice;
  }
}

static void
OnUncapturedError(WGPUDevice const* aDevice,
                  WGPUErrorType aType,
                  WGPUStringView aMessage,
                  void* aUserdata1,
                  void* aUserdata2)
{
  if (aMessage.data && aMessage.length > 0) {
    nsCString msg(aMessage.data, aMessage.length);
    fprintf(stderr, "[WebGPU error type=%d] %s\n", static_cast<int>(aType), msg.get());
  }
}

static void
OnBufferMapDone(WGPUMapAsyncStatus aStatus,
                WGPUStringView aMessage,
                void* aUserdata1,
                void* aUserdata2)
{
  bool* ok = static_cast<bool*>(aUserdata1);
  if (ok) {
    *ok = (aStatus == WGPUMapAsyncStatus_Success);
  }
}

static void
OnPopErrorScope(WGPUPopErrorScopeStatus aStatus,
                WGPUErrorType aType,
                WGPUStringView aMessage,
                void* aUserdata1,
                void* aUserdata2)
{
  if (aType != WGPUErrorType_NoError && aUserdata1) {
    nsCString* out = static_cast<nsCString*>(aUserdata1);
    if (aMessage.data && aMessage.length > 0) {
      out->Assign(aMessage.data, aMessage.length);
    } else {
      out->AssignLiteral("WebGPU error");
    }
  }
}

} // anonymous namespace

/*******************************************************************************
 * GPU
 ******************************************************************************/

GPU::GPU(nsIGlobalObject* aGlobal)
  : mOwner(aGlobal)
  , mInstance(nullptr)
{
  WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
  mInstance = wgpuCreateInstance(&desc);
}

GPU::~GPU()
{
  if (mInstance) {
    wgpuInstanceRelease(mInstance);
    mInstance = nullptr;
  }
}

already_AddRefed<Promise>
GPU::RequestAdapter(JSContext* aCx, JS::Handle<JS::Value> aOptions, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;

  if (!mInstance) {
    promise->MaybeResolve(JS::NullHandleValue);
    return promise.forget();
  }

  WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  if (aOptions.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aOptions.toObject());
    nsCString pref;
    if (GetPropString(aCx, obj, "powerPreference", pref)) {
      if (pref.EqualsLiteral("low-power")) {
        opts.powerPreference = WGPUPowerPreference_LowPower;
      } else if (pref.EqualsLiteral("high-performance")) {
        opts.powerPreference = WGPUPowerPreference_HighPerformance;
      }
    }
    bool forceFallback = false;
    if (GetPropBool(aCx, obj, "forceFallbackAdapter", forceFallback)) {
      opts.forceFallbackAdapter = forceFallback;
    }
  }

  WGPUAdapter rawAdapter = nullptr;
  WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  cb.mode = WGPUCallbackMode_AllowSpontaneous;
  cb.callback = OnAdapterReady;
  cb.userdata1 = &rawAdapter;
  wgpuInstanceRequestAdapter(mInstance, &opts, cb);

  if (!rawAdapter) {
    promise->MaybeResolve(JS::NullHandleValue);
    return promise.forget();
  }

  RefPtr<GPUAdapter> adapter = new GPUAdapter(mOwner, rawAdapter);
  promise->MaybeResolve(adapter);
  return promise.forget();
}

void
GPU::GetPreferredCanvasFormat(nsAString& aRetval) const
{
  aRetval.AssignLiteral("bgra8unorm");
}

void
GPU::GetWgslLanguageFeatures(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval)
{
  nsTArray<nsCString> empty;
  JSObject* setObj = CreateStringSet(aCx, empty);
  if (setObj) {
    aRetval.setObject(*setObj);
  } else {
    aRetval.setUndefined();
  }
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPU, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPU)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPU)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPU)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPU::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUAdapterInfo
 ******************************************************************************/

GPUAdapterInfo::GPUAdapterInfo(nsIGlobalObject* aGlobal,
                               const nsAString& aVendor,
                               const nsAString& aArch,
                               const nsAString& aDevice,
                               const nsAString& aDesc)
  : mOwner(aGlobal), mVendor(aVendor), mArchitecture(aArch), mDevice(aDevice), mDescription(aDesc)
{}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUAdapterInfo, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUAdapterInfo)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUAdapterInfo)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUAdapterInfo)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUAdapterInfo::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUAdapterInfoBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUSupportedLimits
 ******************************************************************************/

GPUSupportedLimits::GPUSupportedLimits(nsIGlobalObject* aGlobal, const WGPULimits& aLimits)
  : mOwner(aGlobal), mLimits(aLimits)
{}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUSupportedLimits, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUSupportedLimits)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUSupportedLimits)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUSupportedLimits)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUSupportedLimits::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUSupportedLimitsBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUAdapter
 ******************************************************************************/

GPUAdapter::GPUAdapter(nsIGlobalObject* aGlobal, WGPUAdapter aAdapter)
  : mOwner(aGlobal)
  , mAdapter(aAdapter)
  , mIsFallback(false)
{
  WGPULimits lim = WGPU_LIMITS_INIT;
  wgpuAdapterGetLimits(mAdapter, &lim);
  mLimits = new GPUSupportedLimits(mOwner, lim);

  WGPUAdapterInfo ai = WGPU_ADAPTER_INFO_INIT;
  wgpuAdapterGetInfo(mAdapter, &ai);
  nsString vendor = ai.vendor.data ? NS_ConvertUTF8toUTF16(ai.vendor.data, ai.vendor.length) : EmptyString();
  nsString arch = ai.architecture.data ? NS_ConvertUTF8toUTF16(ai.architecture.data, ai.architecture.length) : EmptyString();
  nsString dev = ai.device.data ? NS_ConvertUTF8toUTF16(ai.device.data, ai.device.length) : EmptyString();
  nsString desc = ai.description.data ? NS_ConvertUTF8toUTF16(ai.description.data, ai.description.length) : EmptyString();
  mIsFallback = (ai.adapterType == WGPUAdapterType_CPU);
  wgpuAdapterInfoFreeMembers(ai);
  mInfo = new GPUAdapterInfo(mOwner, vendor, arch, dev, desc);
}

GPUAdapter::~GPUAdapter()
{
  if (mAdapter) {
    wgpuAdapterRelease(mAdapter);
    mAdapter = nullptr;
  }
}

void
GPUAdapter::GetFeatures(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval)
{
  nsTArray<nsCString> feats;
  WGPUSupportedFeatures sf = {0};
  wgpuAdapterGetFeatures(mAdapter, &sf);
  for (size_t i = 0; i < sf.featureCount; ++i) {
    const char* name = FeatureNameToString(sf.features[i]);
    if (name) {
      feats.AppendElement(nsCString(name));
    }
  }
  wgpuSupportedFeaturesFreeMembers(sf);
  JSObject* setObj = CreateStringSet(aCx, feats);
  if (setObj) aRetval.setObject(*setObj);
  else aRetval.setUndefined();
}

GPUSupportedLimits* GPUAdapter::Limits() { return mLimits; }
GPUAdapterInfo* GPUAdapter::Info() { return mInfo; }

already_AddRefed<Promise>
GPUAdapter::RequestAdapterInfo(ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;
  promise->MaybeResolve(mInfo);
  return promise.forget();
}

already_AddRefed<Promise>
GPUAdapter::RequestDevice(JSContext* aCx, JS::Handle<JS::Value> aDescriptor, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;

  WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
  desc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;

  std::vector<WGPUFeatureName> reqFeatures;
  WGPULimits reqLimits = WGPU_LIMITS_INIT;
  bool hasLimits = false;

  if (aDescriptor.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDescriptor.toObject());
    JS::Rooted<JSObject*> featObj(aCx);
    if (GetPropObj(aCx, obj, "requiredFeatures", &featObj)) {
      uint32_t len = 0;
      if (JS_GetArrayLength(aCx, featObj, &len)) {
        for (uint32_t i = 0; i < len; ++i) {
          JS::Rooted<JS::Value> v(aCx);
          if (JS_GetElement(aCx, featObj, i, &v) && v.isString()) {
            nsAutoJSString s;
            if (s.init(aCx, v.toString())) {
              WGPUFeatureName fn = ParseFeatureName(NS_ConvertUTF16toUTF8(s));
              if (fn != static_cast<WGPUFeatureName>(0) && wgpuAdapterHasFeature(mAdapter, fn)) {
                reqFeatures.push_back(fn);
              }
            }
          }
        }
      }
    }
    JS::Rooted<JSObject*> limObj(aCx);
    if (GetPropObj(aCx, obj, "requiredLimits", &limObj)) {
      hasLimits = true;
      wgpuAdapterGetLimits(mAdapter, &reqLimits);
      GetPropU32(aCx, limObj, "maxTextureDimension1D", reqLimits.maxTextureDimension1D);
      GetPropU32(aCx, limObj, "maxTextureDimension2D", reqLimits.maxTextureDimension2D);
      GetPropU32(aCx, limObj, "maxTextureDimension3D", reqLimits.maxTextureDimension3D);
      GetPropU32(aCx, limObj, "maxTextureArrayLayers", reqLimits.maxTextureArrayLayers);
      GetPropU32(aCx, limObj, "maxBindGroups", reqLimits.maxBindGroups);
      GetPropU32(aCx, limObj, "maxBindingsPerBindGroup", reqLimits.maxBindingsPerBindGroup);
      GetPropU64(aCx, limObj, "maxUniformBufferBindingSize", reqLimits.maxUniformBufferBindingSize);
      GetPropU64(aCx, limObj, "maxStorageBufferBindingSize", reqLimits.maxStorageBufferBindingSize);
      GetPropU64(aCx, limObj, "maxBufferSize", reqLimits.maxBufferSize);
      GetPropU32(aCx, limObj, "maxVertexBuffers", reqLimits.maxVertexBuffers);
      GetPropU32(aCx, limObj, "maxVertexAttributes", reqLimits.maxVertexAttributes);
      GetPropU32(aCx, limObj, "maxColorAttachments", reqLimits.maxColorAttachments);
      GetPropU32(aCx, limObj, "maxComputeWorkgroupStorageSize", reqLimits.maxComputeWorkgroupStorageSize);
      GetPropU32(aCx, limObj, "maxComputeInvocationsPerWorkgroup", reqLimits.maxComputeInvocationsPerWorkgroup);
      GetPropU32(aCx, limObj, "maxComputeWorkgroupSizeX", reqLimits.maxComputeWorkgroupSizeX);
      GetPropU32(aCx, limObj, "maxComputeWorkgroupSizeY", reqLimits.maxComputeWorkgroupSizeY);
      GetPropU32(aCx, limObj, "maxComputeWorkgroupSizeZ", reqLimits.maxComputeWorkgroupSizeZ);
      GetPropU32(aCx, limObj, "maxComputeWorkgroupsPerDimension", reqLimits.maxComputeWorkgroupsPerDimension);
    }
  }

  if (!reqFeatures.empty()) {
    desc.requiredFeatureCount = reqFeatures.size();
    desc.requiredFeatures = reqFeatures.data();
  }
  if (hasLimits) {
    desc.requiredLimits = &reqLimits;
  }

  WGPUDevice rawDev = nullptr;
  WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  cb.mode = WGPUCallbackMode_AllowSpontaneous;
  cb.callback = OnDeviceReady;
  cb.userdata1 = &rawDev;
  wgpuAdapterRequestDevice(mAdapter, &desc, cb);

  if (!rawDev) {
    promise->MaybeReject(NS_ERROR_DOM_OPERATION_ERR);
    return promise.forget();
  }

  RefPtr<GPUDevice> dev = new GPUDevice(mOwner, rawDev);
  promise->MaybeResolve(dev);
  return promise.forget();
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUAdapter, mOwner, mLimits, mInfo)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUAdapter)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUAdapter)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUAdapter)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUAdapter::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUAdapterBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUDeviceLostInfo / GPUError
 ******************************************************************************/

GPUDeviceLostInfo::GPUDeviceLostInfo(nsIGlobalObject* aGlobal, const nsAString& aReason, const nsAString& aMessage)
  : mOwner(aGlobal), mReason(aReason), mMessage(aMessage)
{}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUDeviceLostInfo, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUDeviceLostInfo)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUDeviceLostInfo)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUDeviceLostInfo)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUDeviceLostInfo::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUDeviceLostInfoBinding::Wrap(aCx, this, aGivenProto);
}

GPUError::GPUError(nsIGlobalObject* aGlobal, const nsAString& aMessage)
  : mOwner(aGlobal), mMessage(aMessage)
{}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUError, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUError)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUError)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUError)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUError::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUErrorBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUDevice
 ******************************************************************************/

GPUDevice::GPUDevice(nsIGlobalObject* aGlobal, WGPUDevice aDevice)
  : DOMEventTargetHelper(aGlobal)
  , mOwner(aGlobal)
  , mDevice(aDevice)
{
  WGPULimits lim = WGPU_LIMITS_INIT;
  wgpuDeviceGetLimits(mDevice, &lim);
  mLimits = new GPUSupportedLimits(mOwner, lim);
  WGPUQueue q = wgpuDeviceGetQueue(mDevice);
  mQueue = new GPUQueue(mOwner, mDevice, q);
}

GPUDevice::~GPUDevice()
{
  mQueue = nullptr;
  if (mDevice) {
    wgpuDeviceRelease(mDevice);
    mDevice = nullptr;
  }
}

void
GPUDevice::GetFeatures(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval)
{
  nsTArray<nsCString> feats;
  WGPUSupportedFeatures sf = {0};
  wgpuDeviceGetFeatures(mDevice, &sf);
  for (size_t i = 0; i < sf.featureCount; ++i) {
    const char* name = FeatureNameToString(sf.features[i]);
    if (name) feats.AppendElement(nsCString(name));
  }
  wgpuSupportedFeaturesFreeMembers(sf);
  JSObject* setObj = CreateStringSet(aCx, feats);
  if (setObj) aRetval.setObject(*setObj);
  else aRetval.setUndefined();
}

GPUSupportedLimits* GPUDevice::Limits() { return mLimits; }
GPUQueue* GPUDevice::Queue() { return mQueue; }

already_AddRefed<Promise>
GPUDevice::GetLost(ErrorResult& aRv)
{
  if (!mLostPromise) {
    mLostPromise = Promise::Create(mOwner, aRv);
  }
  RefPtr<Promise> p = mLostPromise;
  return p.forget();
}

void
GPUDevice::Destroy()
{
  if (mDevice) {
    wgpuDeviceDestroy(mDevice);
  }
  if (mLostPromise) {
    RefPtr<GPUDeviceLostInfo> info = new GPUDeviceLostInfo(
      mOwner, NS_LITERAL_STRING("destroyed"), NS_LITERAL_STRING("Device destroyed"));
    mLostPromise->MaybeResolve(info);
  }
}

already_AddRefed<GPUBuffer>
GPUDevice::CreateBuffer(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  nsCString label;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  uint64_t size = 0;
  uint32_t usage = 0;
  bool mappedAtCreation = false;
  GetPropU64(aCx, obj, "size", size);
  GetPropU32(aCx, obj, "usage", usage);
  GetPropBool(aCx, obj, "mappedAtCreation", mappedAtCreation);
  desc.size = size;
  desc.usage = static_cast<WGPUBufferUsage>(usage);
  desc.mappedAtCreation = mappedAtCreation;

  WGPUBuffer raw = wgpuDeviceCreateBuffer(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUBuffer> buf = new GPUBuffer(mOwner, mDevice, raw, size, usage, mappedAtCreation);
  return buf.forget();
}

already_AddRefed<GPUTexture>
GPUDevice::CreateTexture(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
  nsCString label, fmtStr, dimStr("2d");
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  JS::Rooted<JS::Value> sizeVal(aCx);
  if (GetPropVal(aCx, obj, "size", &sizeVal)) {
    desc.size = ParseExtent3D(aCx, sizeVal);
  }
  GetPropU32(aCx, obj, "mipLevelCount", desc.mipLevelCount);
  GetPropU32(aCx, obj, "sampleCount", desc.sampleCount);
  if (GetPropString(aCx, obj, "dimension", dimStr)) {
    desc.dimension = ParseTextureDimension(dimStr);
  } else {
    desc.dimension = WGPUTextureDimension_2D;
  }
  if (GetPropString(aCx, obj, "format", fmtStr)) {
    desc.format = ParseTextureFormat(fmtStr);
  }
  uint32_t usage = 0;
  GetPropU32(aCx, obj, "usage", usage);
  desc.usage = static_cast<WGPUTextureUsage>(usage);

  std::vector<WGPUTextureFormat> viewFormats;
  JS::Rooted<JSObject*> vfObj(aCx);
  if (GetPropObj(aCx, obj, "viewFormats", &vfObj)) {
    uint32_t len = 0;
    if (JS_GetArrayLength(aCx, vfObj, &len)) {
      for (uint32_t i = 0; i < len; ++i) {
        JS::Rooted<JS::Value> v(aCx);
        if (JS_GetElement(aCx, vfObj, i, &v) && v.isString()) {
          nsAutoJSString s;
          if (s.init(aCx, v.toString())) {
            WGPUTextureFormat f = ParseTextureFormat(NS_ConvertUTF16toUTF8(s));
            if (f != WGPUTextureFormat_Undefined) viewFormats.push_back(f);
          }
        }
      }
    }
  }
  if (!viewFormats.empty()) {
    desc.viewFormatCount = viewFormats.size();
    desc.viewFormats = viewFormats.data();
  }

  WGPUTexture raw = wgpuDeviceCreateTexture(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUTexture> tex = new GPUTexture(
    mOwner, raw, desc.size.width, desc.size.height, desc.size.depthOrArrayLayers,
    desc.mipLevelCount, desc.sampleCount,
    NS_ConvertUTF8toUTF16(dimStr), NS_ConvertUTF8toUTF16(fmtStr), usage);
  return tex.forget();
}

already_AddRefed<GPUSampler>
GPUDevice::CreateSampler(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
  nsCString label, s;
  if (aDesc.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
    if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
    if (GetPropString(aCx, obj, "addressModeU", s)) desc.addressModeU = ParseAddressMode(s);
    if (GetPropString(aCx, obj, "addressModeV", s)) desc.addressModeV = ParseAddressMode(s);
    if (GetPropString(aCx, obj, "addressModeW", s)) desc.addressModeW = ParseAddressMode(s);
    if (GetPropString(aCx, obj, "magFilter", s)) desc.magFilter = ParseFilterMode(s);
    if (GetPropString(aCx, obj, "minFilter", s)) desc.minFilter = ParseFilterMode(s);
    if (GetPropString(aCx, obj, "mipmapFilter", s)) desc.mipmapFilter = ParseMipmapFilterMode(s);
    GetPropFloat(aCx, obj, "lodMinClamp", desc.lodMinClamp);
    GetPropFloat(aCx, obj, "lodMaxClamp", desc.lodMaxClamp);
    if (GetPropString(aCx, obj, "compare", s)) desc.compare = ParseCompareFunc(s);
    uint32_t maxAniso = 1;
    if (GetPropU32(aCx, obj, "maxAnisotropy", maxAniso)) desc.maxAnisotropy = static_cast<uint16_t>(maxAniso);
  }
  WGPUSampler raw = wgpuDeviceCreateSampler(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUSampler> ret = new GPUSampler(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUBindGroupLayout>
GPUDevice::CreateBindGroupLayout(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  nsCString label;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  std::vector<WGPUBindGroupLayoutEntry> entries;
  JS::Rooted<JSObject*> entriesObj(aCx);
  if (GetPropObj(aCx, obj, "entries", &entriesObj)) {
    uint32_t len = 0;
    if (JS_GetArrayLength(aCx, entriesObj, &len)) {
      entries.reserve(len);
      for (uint32_t i = 0; i < len; ++i) {
        JS::Rooted<JS::Value> ev(aCx);
        if (!JS_GetElement(aCx, entriesObj, i, &ev) || !ev.isObject()) continue;
        JS::Rooted<JSObject*> eo(aCx, &ev.toObject());
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        GetPropU32(aCx, eo, "binding", e.binding);
        uint32_t vis = 0;
        GetPropU32(aCx, eo, "visibility", vis);
        e.visibility = static_cast<WGPUShaderStage>(vis);

        JS::Rooted<JSObject*> sub(aCx);
        nsCString s;
        if (GetPropObj(aCx, eo, "buffer", &sub)) {
          e.buffer.type = WGPUBufferBindingType_Uniform;
          if (GetPropString(aCx, sub, "type", s)) e.buffer.type = ParseBufferBindingType(s);
          bool dyn = false;
          if (GetPropBool(aCx, sub, "hasDynamicOffset", dyn)) e.buffer.hasDynamicOffset = dyn;
          GetPropU64(aCx, sub, "minBindingSize", e.buffer.minBindingSize);
        }
        if (GetPropObj(aCx, eo, "sampler", &sub)) {
          e.sampler.type = WGPUSamplerBindingType_Filtering;
          if (GetPropString(aCx, sub, "type", s)) e.sampler.type = ParseSamplerBindingType(s);
        }
        if (GetPropObj(aCx, eo, "texture", &sub)) {
          e.texture.sampleType = WGPUTextureSampleType_Float;
          e.texture.viewDimension = WGPUTextureViewDimension_2D;
          if (GetPropString(aCx, sub, "sampleType", s)) e.texture.sampleType = ParseTextureSampleType(s);
          if (GetPropString(aCx, sub, "viewDimension", s)) e.texture.viewDimension = ParseTextureViewDimension(s);
          bool ms = false;
          if (GetPropBool(aCx, sub, "multisampled", ms)) e.texture.multisampled = ms;
        }
        if (GetPropObj(aCx, eo, "storageTexture", &sub)) {
          e.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
          e.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
          if (GetPropString(aCx, sub, "access", s)) e.storageTexture.access = ParseStorageTextureAccess(s);
          if (GetPropString(aCx, sub, "format", s)) e.storageTexture.format = ParseTextureFormat(s);
          if (GetPropString(aCx, sub, "viewDimension", s)) e.storageTexture.viewDimension = ParseTextureViewDimension(s);
        }
        entries.push_back(e);
      }
    }
  }

  desc.entryCount = entries.size();
  desc.entries = entries.empty() ? nullptr : entries.data();

  WGPUBindGroupLayout raw = wgpuDeviceCreateBindGroupLayout(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUBindGroupLayout> ret = new GPUBindGroupLayout(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUPipelineLayout>
GPUDevice::CreatePipelineLayout(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUPipelineLayoutDescriptor desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  nsCString label;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  std::vector<WGPUBindGroupLayout> layouts;
  JS::Rooted<JSObject*> arrObj(aCx);
  if (GetPropObj(aCx, obj, "bindGroupLayouts", &arrObj)) {
    uint32_t len = 0;
    if (JS_GetArrayLength(aCx, arrObj, &len)) {
      for (uint32_t i = 0; i < len; ++i) {
        JS::Rooted<JS::Value> v(aCx);
        if (JS_GetElement(aCx, arrObj, i, &v) && v.isObject()) {
          JS::Rooted<JSObject*> lo(aCx, &v.toObject());
          RefPtr<GPUBindGroupLayout> bgl;
          if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUBindGroupLayout, lo, bgl)) && bgl) {
            layouts.push_back(bgl->Raw());
          }
        }
      }
    }
  }
  desc.bindGroupLayoutCount = layouts.size();
  desc.bindGroupLayouts = layouts.empty() ? nullptr : layouts.data();

  WGPUPipelineLayout raw = wgpuDeviceCreatePipelineLayout(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUPipelineLayout> ret = new GPUPipelineLayout(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUBindGroup>
GPUDevice::CreateBindGroup(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  nsCString label;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  JS::Rooted<JSObject*> layoutObj(aCx);
  if (!GetPropObj(aCx, obj, "layout", &layoutObj)) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  RefPtr<GPUBindGroupLayout> bgl;
  if (NS_FAILED(UNWRAP_OBJECT(GPUBindGroupLayout, layoutObj, bgl)) || !bgl) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return nullptr;
  }
  desc.layout = bgl->Raw();

  std::vector<WGPUBindGroupEntry> entries;
  JS::Rooted<JSObject*> entriesObj(aCx);
  if (GetPropObj(aCx, obj, "entries", &entriesObj)) {
    uint32_t len = 0;
    if (JS_GetArrayLength(aCx, entriesObj, &len)) {
      for (uint32_t i = 0; i < len; ++i) {
        JS::Rooted<JS::Value> ev(aCx);
        if (!JS_GetElement(aCx, entriesObj, i, &ev) || !ev.isObject()) continue;
        JS::Rooted<JSObject*> eo(aCx, &ev.toObject());
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        GetPropU32(aCx, eo, "binding", e.binding);
        JS::Rooted<JSObject*> resObj(aCx);
        if (!GetPropObj(aCx, eo, "resource", &resObj)) continue;

        RefPtr<GPUSampler> samp;
        RefPtr<GPUTextureView> view;
        if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUSampler, resObj, samp)) && samp) {
          e.sampler = samp->Raw();
        } else if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUTextureView, resObj, view)) && view) {
          e.textureView = view->Raw();
        } else {
          // GPUBufferBinding: { buffer, offset, size }
          JS::Rooted<JSObject*> bufObj(aCx);
          if (GetPropObj(aCx, resObj, "buffer", &bufObj)) {
            RefPtr<GPUBuffer> buf;
            if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUBuffer, bufObj, buf)) && buf) {
              e.buffer = buf->Raw();
              e.offset = 0;
              e.size = WGPU_WHOLE_SIZE;
              GetPropU64(aCx, resObj, "offset", e.offset);
              GetPropU64(aCx, resObj, "size", e.size);
            }
          }
        }
        entries.push_back(e);
      }
    }
  }
  desc.entryCount = entries.size();
  desc.entries = entries.empty() ? nullptr : entries.data();

  WGPUBindGroup raw = wgpuDeviceCreateBindGroup(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUBindGroup> ret = new GPUBindGroup(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUShaderModule>
GPUDevice::CreateShaderModule(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  nsCString label, code;
  GetPropString(aCx, obj, "label", label);
  GetPropString(aCx, obj, "code", code);

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = ToWGPUString(code);

  WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  desc.nextInChain = &wgsl.chain;
  if (!label.IsEmpty()) desc.label = ToWGPUString(label);

  WGPUShaderModule raw = wgpuDeviceCreateShaderModule(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUShaderModule> ret = new GPUShaderModule(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUComputePipeline>
GPUDevice::CreateComputePipeline(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  nsCString label, ep;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  JS::Rooted<JSObject*> layoutObj(aCx);
  if (GetPropObj(aCx, obj, "layout", &layoutObj)) {
    RefPtr<GPUPipelineLayout> pl;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUPipelineLayout, layoutObj, pl)) && pl) {
      desc.layout = pl->Raw();
    }
  }

  JS::Rooted<JSObject*> compObj(aCx);
  if (GetPropObj(aCx, obj, "compute", &compObj)) {
    JS::Rooted<JSObject*> modObj(aCx);
    if (GetPropObj(aCx, compObj, "module", &modObj)) {
      RefPtr<GPUShaderModule> sm;
      if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUShaderModule, modObj, sm)) && sm) {
        desc.compute.module = sm->Raw();
      }
    }
    if (GetPropString(aCx, compObj, "entryPoint", ep)) {
      desc.compute.entryPoint = ToWGPUString(ep);
    }
  }

  WGPUComputePipeline raw = wgpuDeviceCreateComputePipeline(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUComputePipeline> ret = new GPUComputePipeline(mOwner, raw);
  return ret.forget();
}

already_AddRefed<Promise>
GPUDevice::CreateComputePipelineAsync(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;
  ErrorResult rv;
  RefPtr<GPUComputePipeline> pipe = CreateComputePipeline(aCx, aDesc, rv);
  if (rv.Failed() || !pipe) {
    rv.SuppressException();
    promise->MaybeReject(NS_ERROR_DOM_OPERATION_ERR);
  } else {
    promise->MaybeResolve(pipe);
  }
  return promise.forget();
}

already_AddRefed<GPURenderPipeline>
GPUDevice::CreateRenderPipeline(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  nsCString label, vertEp, fragEp, s;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  JS::Rooted<JSObject*> layoutObj(aCx);
  if (GetPropObj(aCx, obj, "layout", &layoutObj)) {
    RefPtr<GPUPipelineLayout> pl;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUPipelineLayout, layoutObj, pl)) && pl) {
      desc.layout = pl->Raw();
    }
  }

  // Vertex state
  std::vector<WGPUVertexBufferLayout> vBuffers;
  std::vector<std::vector<WGPUVertexAttribute>> vAttrsStorage;
  JS::Rooted<JSObject*> vertObj(aCx);
  if (GetPropObj(aCx, obj, "vertex", &vertObj)) {
    JS::Rooted<JSObject*> modObj(aCx);
    if (GetPropObj(aCx, vertObj, "module", &modObj)) {
      RefPtr<GPUShaderModule> sm;
      if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUShaderModule, modObj, sm)) && sm) {
        desc.vertex.module = sm->Raw();
      }
    }
    if (GetPropString(aCx, vertObj, "entryPoint", vertEp)) {
      desc.vertex.entryPoint = ToWGPUString(vertEp);
    }
    JS::Rooted<JSObject*> bufsObj(aCx);
    if (GetPropObj(aCx, vertObj, "buffers", &bufsObj)) {
      uint32_t len = 0;
      if (JS_GetArrayLength(aCx, bufsObj, &len)) {
        vAttrsStorage.resize(len);
        vBuffers.resize(len, WGPU_VERTEX_BUFFER_LAYOUT_INIT);
        for (uint32_t i = 0; i < len; ++i) {
          JS::Rooted<JS::Value> bv(aCx);
          if (!JS_GetElement(aCx, bufsObj, i, &bv) || !bv.isObject()) {
            vBuffers[i].stepMode = WGPUVertexStepMode_Undefined;
            continue;
          }
          JS::Rooted<JSObject*> bo(aCx, &bv.toObject());
          GetPropU64(aCx, bo, "arrayStride", vBuffers[i].arrayStride);
          vBuffers[i].stepMode = WGPUVertexStepMode_Vertex;
          if (GetPropString(aCx, bo, "stepMode", s)) vBuffers[i].stepMode = ParseStepMode(s);
          JS::Rooted<JSObject*> attrsObj(aCx);
          if (GetPropObj(aCx, bo, "attributes", &attrsObj)) {
            uint32_t alen = 0;
            if (JS_GetArrayLength(aCx, attrsObj, &alen)) {
              for (uint32_t j = 0; j < alen; ++j) {
                JS::Rooted<JS::Value> av(aCx);
                if (!JS_GetElement(aCx, attrsObj, j, &av) || !av.isObject()) continue;
                JS::Rooted<JSObject*> ao(aCx, &av.toObject());
                WGPUVertexAttribute attr = WGPU_VERTEX_ATTRIBUTE_INIT;
                if (GetPropString(aCx, ao, "format", s)) attr.format = ParseVertexFormat(s);
                GetPropU64(aCx, ao, "offset", attr.offset);
                GetPropU32(aCx, ao, "shaderLocation", attr.shaderLocation);
                vAttrsStorage[i].push_back(attr);
              }
            }
          }
          vBuffers[i].attributeCount = vAttrsStorage[i].size();
          vBuffers[i].attributes = vAttrsStorage[i].empty() ? nullptr : vAttrsStorage[i].data();
        }
        desc.vertex.bufferCount = vBuffers.size();
        desc.vertex.buffers = vBuffers.data();
      }
    }
  }

  // Primitive state
  JS::Rooted<JSObject*> primObj(aCx);
  if (GetPropObj(aCx, obj, "primitive", &primObj)) {
    if (GetPropString(aCx, primObj, "topology", s)) desc.primitive.topology = ParseTopology(s);
    if (GetPropString(aCx, primObj, "stripIndexFormat", s)) desc.primitive.stripIndexFormat = ParseIndexFormat(s);
    if (GetPropString(aCx, primObj, "frontFace", s)) desc.primitive.frontFace = ParseFrontFace(s);
    if (GetPropString(aCx, primObj, "cullMode", s)) desc.primitive.cullMode = ParseCullMode(s);
  }

  // DepthStencil state
  WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
  JS::Rooted<JSObject*> dsObj(aCx);
  if (GetPropObj(aCx, obj, "depthStencil", &dsObj)) {
    if (GetPropString(aCx, dsObj, "format", s)) ds.format = ParseTextureFormat(s);
    bool dwe = false;
    if (GetPropBool(aCx, dsObj, "depthWriteEnabled", dwe)) {
      ds.depthWriteEnabled = dwe ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    }
    if (GetPropString(aCx, dsObj, "depthCompare", s)) ds.depthCompare = ParseCompareFunc(s);
    GetPropU32(aCx, dsObj, "stencilReadMask", ds.stencilReadMask);
    GetPropU32(aCx, dsObj, "stencilWriteMask", ds.stencilWriteMask);
    GetPropI32(aCx, dsObj, "depthBias", ds.depthBias);
    GetPropFloat(aCx, dsObj, "depthBiasSlopeScale", ds.depthBiasSlopeScale);
    GetPropFloat(aCx, dsObj, "depthBiasClamp", ds.depthBiasClamp);

    auto parseFace = [&](const char* prop, WGPUStencilFaceState& face) {
      JS::Rooted<JSObject*> fo(aCx);
      if (GetPropObj(aCx, dsObj, prop, &fo)) {
        if (GetPropString(aCx, fo, "compare", s)) face.compare = ParseCompareFunc(s);
        if (GetPropString(aCx, fo, "failOp", s)) face.failOp = ParseStencilOp(s);
        if (GetPropString(aCx, fo, "depthFailOp", s)) face.depthFailOp = ParseStencilOp(s);
        if (GetPropString(aCx, fo, "passOp", s)) face.passOp = ParseStencilOp(s);
      }
    };
    parseFace("stencilFront", ds.stencilFront);
    parseFace("stencilBack", ds.stencilBack);
    desc.depthStencil = &ds;
  }

  // Multisample state
  JS::Rooted<JSObject*> msObj(aCx);
  if (GetPropObj(aCx, obj, "multisample", &msObj)) {
    GetPropU32(aCx, msObj, "count", desc.multisample.count);
    GetPropU32(aCx, msObj, "mask", desc.multisample.mask);
    bool a2c = false;
    if (GetPropBool(aCx, msObj, "alphaToCoverageEnabled", a2c)) desc.multisample.alphaToCoverageEnabled = a2c;
  }

  // Fragment state
  WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
  std::vector<WGPUColorTargetState> targets;
  std::vector<WGPUBlendState> blends;
  JS::Rooted<JSObject*> fragObj(aCx);
  if (GetPropObj(aCx, obj, "fragment", &fragObj)) {
    JS::Rooted<JSObject*> modObj(aCx);
    if (GetPropObj(aCx, fragObj, "module", &modObj)) {
      RefPtr<GPUShaderModule> sm;
      if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUShaderModule, modObj, sm)) && sm) {
        frag.module = sm->Raw();
      }
    }
    if (GetPropString(aCx, fragObj, "entryPoint", fragEp)) {
      frag.entryPoint = ToWGPUString(fragEp);
    }
    JS::Rooted<JSObject*> tObj(aCx);
    if (GetPropObj(aCx, fragObj, "targets", &tObj)) {
      uint32_t len = 0;
      if (JS_GetArrayLength(aCx, tObj, &len)) {
        targets.resize(len, WGPU_COLOR_TARGET_STATE_INIT);
        blends.resize(len, WGPU_BLEND_STATE_INIT);
        for (uint32_t i = 0; i < len; ++i) {
          JS::Rooted<JS::Value> tv(aCx);
          if (!JS_GetElement(aCx, tObj, i, &tv) || !tv.isObject()) continue;
          JS::Rooted<JSObject*> to(aCx, &tv.toObject());
          if (GetPropString(aCx, to, "format", s)) targets[i].format = ParseTextureFormat(s);
          uint32_t wm = WGPUColorWriteMask_All;
          if (GetPropU32(aCx, to, "writeMask", wm)) targets[i].writeMask = static_cast<WGPUColorWriteMask>(wm);
          JS::Rooted<JSObject*> blendObj(aCx);
          if (GetPropObj(aCx, to, "blend", &blendObj)) {
            auto parseComp = [&](const char* prop, WGPUBlendComponent& bc) {
              JS::Rooted<JSObject*> co(aCx);
              if (GetPropObj(aCx, blendObj, prop, &co)) {
                if (GetPropString(aCx, co, "operation", s)) bc.operation = ParseBlendOp(s);
                if (GetPropString(aCx, co, "srcFactor", s)) bc.srcFactor = ParseBlendFactor(s);
                if (GetPropString(aCx, co, "dstFactor", s)) bc.dstFactor = ParseBlendFactor(s);
              }
            };
            parseComp("color", blends[i].color);
            parseComp("alpha", blends[i].alpha);
            targets[i].blend = &blends[i];
          }
        }
        frag.targetCount = targets.size();
        frag.targets = targets.data();
      }
    }
    desc.fragment = &frag;
  }

  WGPURenderPipeline raw = wgpuDeviceCreateRenderPipeline(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPURenderPipeline> ret = new GPURenderPipeline(mOwner, raw);
  return ret.forget();
}

already_AddRefed<Promise>
GPUDevice::CreateRenderPipelineAsync(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;
  ErrorResult rv;
  RefPtr<GPURenderPipeline> pipe = CreateRenderPipeline(aCx, aDesc, rv);
  if (rv.Failed() || !pipe) {
    rv.SuppressException();
    promise->MaybeReject(NS_ERROR_DOM_OPERATION_ERR);
  } else {
    promise->MaybeResolve(pipe);
  }
  return promise.forget();
}

already_AddRefed<GPUCommandEncoder>
GPUDevice::CreateCommandEncoder(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  WGPUCommandEncoderDescriptor desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  nsCString label;
  if (aDesc.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
    if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  }
  WGPUCommandEncoder raw = wgpuDeviceCreateCommandEncoder(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUCommandEncoder> ret = new GPUCommandEncoder(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPURenderBundleEncoder>
GPUDevice::CreateRenderBundleEncoder(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPURenderBundleEncoderDescriptor desc = WGPU_RENDER_BUNDLE_ENCODER_DESCRIPTOR_INIT;
  nsCString label, s;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  std::vector<WGPUTextureFormat> colorFormats;
  JS::Rooted<JSObject*> cfObj(aCx);
  if (GetPropObj(aCx, obj, "colorFormats", &cfObj)) {
    uint32_t len = 0;
    if (JS_GetArrayLength(aCx, cfObj, &len)) {
      for (uint32_t i = 0; i < len; ++i) {
        JS::Rooted<JS::Value> v(aCx);
        if (JS_GetElement(aCx, cfObj, i, &v) && v.isString()) {
          nsAutoJSString fs;
          if (fs.init(aCx, v.toString())) {
            colorFormats.push_back(ParseTextureFormat(NS_ConvertUTF16toUTF8(fs)));
          }
        }
      }
    }
  }
  desc.colorFormatCount = colorFormats.size();
  desc.colorFormats = colorFormats.empty() ? nullptr : colorFormats.data();
  if (GetPropString(aCx, obj, "depthStencilFormat", s)) desc.depthStencilFormat = ParseTextureFormat(s);
  GetPropU32(aCx, obj, "sampleCount", desc.sampleCount);
  bool dro = false, sro = false;
  if (GetPropBool(aCx, obj, "depthReadOnly", dro)) desc.depthReadOnly = dro;
  if (GetPropBool(aCx, obj, "stencilReadOnly", sro)) desc.stencilReadOnly = sro;

  WGPURenderBundleEncoder raw = wgpuDeviceCreateRenderBundleEncoder(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPURenderBundleEncoder> ret = new GPURenderBundleEncoder(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUQuerySet>
GPUDevice::CreateQuerySet(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPUQuerySetDescriptor desc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
  nsCString label, typeStr("occlusion");
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  GetPropString(aCx, obj, "type", typeStr);
  desc.type = typeStr.EqualsLiteral("timestamp") ? WGPUQueryType_Timestamp : WGPUQueryType_Occlusion;
  GetPropU32(aCx, obj, "count", desc.count);

  WGPUQuerySet raw = wgpuDeviceCreateQuerySet(mDevice, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUQuerySet> ret = new GPUQuerySet(mOwner, raw, NS_ConvertUTF8toUTF16(typeStr), desc.count);
  return ret.forget();
}

void
GPUDevice::PushErrorScope(const nsAString& aFilter)
{
  WGPUErrorFilter f = WGPUErrorFilter_Validation;
  if (aFilter.EqualsLiteral("out-of-memory")) f = WGPUErrorFilter_OutOfMemory;
  else if (aFilter.EqualsLiteral("internal")) f = WGPUErrorFilter_Internal;
  wgpuDevicePushErrorScope(mDevice, f);
}

already_AddRefed<Promise>
GPUDevice::PopErrorScope(ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;
  nsCString errMsg;
  WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  cb.mode = WGPUCallbackMode_AllowSpontaneous;
  cb.callback = OnPopErrorScope;
  cb.userdata1 = &errMsg;
  wgpuDevicePopErrorScope(mDevice, cb);
  wgpuDevicePoll(mDevice, false, nullptr);
  if (errMsg.IsEmpty()) {
    promise->MaybeResolve(JS::NullHandleValue);
  } else {
    RefPtr<GPUError> err = new GPUError(mOwner, NS_ConvertUTF8toUTF16(errMsg));
    promise->MaybeResolve(err);
  }
  return promise.forget();
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(GPUDevice, DOMEventTargetHelper, mOwner, mLimits, mQueue, mLostPromise)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUDevice)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)
NS_IMPL_ADDREF_INHERITED(GPUDevice, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(GPUDevice, DOMEventTargetHelper)

JSObject*
GPUDevice::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUDeviceBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUQueue
 ******************************************************************************/

GPUQueue::GPUQueue(nsIGlobalObject* aGlobal, WGPUDevice aDevice, WGPUQueue aQueue)
  : mOwner(aGlobal), mDevice(aDevice), mQueue(aQueue)
{}

GPUQueue::~GPUQueue()
{
  if (mQueue) {
    wgpuQueueRelease(mQueue);
    mQueue = nullptr;
  }
}

void
GPUQueue::Submit(const Sequence<OwningNonNull<GPUCommandBuffer>>& aCommandBuffers)
{
  std::vector<WGPUCommandBuffer> cmds;
  cmds.reserve(aCommandBuffers.Length());
  for (uint32_t i = 0; i < aCommandBuffers.Length(); ++i) {
    if (aCommandBuffers[i]->Raw()) {
      cmds.push_back(aCommandBuffers[i]->Raw());
    }
  }
  wgpuQueueSubmit(mQueue, cmds.size(), cmds.empty() ? nullptr : cmds.data());
  PresentAllWebGPUCanvasContextsForDevice(mDevice);
}

already_AddRefed<Promise>
GPUQueue::OnSubmittedWorkDone(ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;
  wgpuDevicePoll(mDevice, false, nullptr);
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

static bool
ExtractAnyBufferSource(JSContext* aCx,
                       JS::Handle<JS::Value> aVal,
                       const uint8_t*& aOutPtr,
                       size_t& aOutByteLen,
                       size_t& aOutElemSize)
{
  if (!aVal.isObject()) {
    return false;
  }
  JS::Rooted<JSObject*> obj(aCx, js::CheckedUnwrap(&aVal.toObject()));
  if (!obj) {
    return false;
  }
  JS::AutoCheckCannotGC nogc;
  bool isShared = false;
  if (JS_IsArrayBufferViewObject(obj)) {
    aOutByteLen = JS_GetArrayBufferViewByteLength(obj);
    aOutPtr = static_cast<const uint8_t*>(JS_GetArrayBufferViewData(obj, &isShared, nogc));
    aOutElemSize = 1;
    if (JS_IsTypedArrayObject(obj)) {
      switch (JS_GetArrayBufferViewType(obj)) {
        case js::Scalar::Int16:
        case js::Scalar::Uint16:
          aOutElemSize = 2;
          break;
        case js::Scalar::Int32:
        case js::Scalar::Uint32:
        case js::Scalar::Float32:
          aOutElemSize = 4;
          break;
        case js::Scalar::Float64:
        case js::Scalar::BigInt64:
        case js::Scalar::BigUint64:
          aOutElemSize = 8;
          break;
        default:
          aOutElemSize = 1;
          break;
      }
    }
    return aOutPtr != nullptr || aOutByteLen == 0;
  }
  if (JS_IsArrayBufferObject(obj)) {
    aOutByteLen = JS_GetArrayBufferByteLength(obj);
    aOutPtr = JS_GetArrayBufferData(obj, &isShared, nogc);
    aOutElemSize = 1;
    return aOutPtr != nullptr || aOutByteLen == 0;
  }
  if (JS_IsSharedArrayBufferObject(obj)) {
    aOutByteLen = JS_GetSharedArrayBufferByteLength(obj);
    aOutPtr = JS_GetSharedArrayBufferData(obj, &isShared, nogc);
    aOutElemSize = 1;
    return aOutPtr != nullptr || aOutByteLen == 0;
  }
  return false;
}

void
GPUQueue::WriteBuffer(JSContext* aCx,
                      GPUBuffer& aBuffer,
                      uint64_t aBufferOffset,
                      JS::Handle<JS::Value> aData,
                      uint64_t aDataOffset,
                      const Optional<uint64_t>& aSize,
                      ErrorResult& aRv)
{
  const uint8_t* ptr = nullptr;
  size_t byteLen = 0, elemSize = 1;
  if (!ExtractAnyBufferSource(aCx, aData, ptr, byteLen, elemSize)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }
  uint64_t byteOff = aDataOffset * elemSize;
  if (byteOff > byteLen) {
    aRv.Throw(NS_ERROR_DOM_OPERATION_ERR);
    return;
  }
  uint64_t writeBytes = aSize.WasPassed() ? (aSize.Value() * elemSize) : (byteLen - byteOff);
  if (byteOff + writeBytes > byteLen) {
    writeBytes = byteLen - byteOff;
  }
  if (writeBytes > 0) {
    wgpuQueueWriteBuffer(mQueue, aBuffer.Raw(), aBufferOffset, ptr + byteOff, static_cast<size_t>(writeBytes));
  }
}

void
GPUQueue::WriteTexture(JSContext* aCx,
                       JS::Handle<JS::Value> aDestination,
                       JS::Handle<JS::Value> aData,
                       JS::Handle<JS::Value> aDataLayout,
                       JS::Handle<JS::Value> aSize,
                       ErrorResult& aRv)
{
  WGPUTexelCopyTextureInfo dst;
  WGPUTexelCopyBufferLayout layout;
  if (!ParseTexelCopyTextureInfo(aCx, aDestination, dst)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }
  ParseTexelCopyBufferLayout(aCx, aDataLayout, layout);
  WGPUExtent3D ext = ParseExtent3D(aCx, aSize);

  const uint8_t* ptr = nullptr;
  size_t byteLen = 0, elemSize = 1;
  if (!ExtractAnyBufferSource(aCx, aData, ptr, byteLen, elemSize)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }
  wgpuQueueWriteTexture(mQueue, &dst, ptr, byteLen, &layout, &ext);
}

void
GPUQueue::CopyExternalImageToTexture(JSContext* aCx,
                                     JS::Handle<JS::Value> aSource,
                                     JS::Handle<JS::Value> aDestination,
                                     JS::Handle<JS::Value> aCopySize,
                                     ErrorResult& aRv)
{
  if (!aSource.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return; }
  JS::Rooted<JSObject*> srcDict(aCx, &aSource.toObject());
  JS::Rooted<JSObject*> srcObj(aCx);
  if (!GetPropObj(aCx, srcDict, "source", &srcObj)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }

  WGPUTexelCopyTextureInfo dst;
  if (!ParseTexelCopyTextureInfo(aCx, aDestination, dst)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }
  WGPUExtent3D ext = ParseExtent3D(aCx, aCopySize);

  RefPtr<gfx::SourceSurface> surf;
  RefPtr<ImageBitmap> bmp;
  RefPtr<HTMLCanvasElement> canvas;
  if (NS_SUCCEEDED(UNWRAP_OBJECT(ImageBitmap, srcObj, bmp)) && bmp) {
    surf = bmp->PrepareForDrawTarget(nullptr);
  } else if (NS_SUCCEEDED(UNWRAP_OBJECT(HTMLCanvasElement, srcObj, canvas)) && canvas) {
    surf = canvas->GetSurfaceSnapshot();
  }
  if (!surf) {
    return;
  }
  RefPtr<gfx::DataSourceSurface> data = surf->GetDataSurface();
  if (!data) return;
  gfx::DataSourceSurface::ScopedMap map(data, gfx::DataSourceSurface::READ);
  if (!map.IsMapped()) return;

  uint32_t w = std::min<uint32_t>(ext.width, data->GetSize().width);
  uint32_t h = std::min<uint32_t>(ext.height, data->GetSize().height);
  if (w == 0 || h == 0) return;

  // Convert BGRA to RGBA for standard rgba8unorm upload.
  std::vector<uint8_t> rgba(w * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* srcRow = map.GetData() + y * map.GetStride();
    uint8_t* dstRow = rgba.data() + y * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
      dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
      dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
      dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
    }
  }

  WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
  layout.bytesPerRow = w * 4;
  layout.rowsPerImage = h;
  WGPUExtent3D copyExt = { w, h, 1 };
  wgpuQueueWriteTexture(mQueue, &dst, rgba.data(), rgba.size(), &layout, &copyExt);
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUQueue, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUQueue)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUQueue)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUQueue)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUQueue::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUQueueBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUBuffer
 ******************************************************************************/

GPUBuffer::GPUBuffer(nsIGlobalObject* aGlobal, WGPUDevice aDevice, WGPUBuffer aBuffer,
                     uint64_t aSize, uint32_t aUsage, bool aMappedAtCreation)
  : mOwner(aGlobal)
  , mDevice(aDevice)
  , mBuffer(aBuffer)
  , mSize(aSize)
  , mUsage(aUsage)
  , mMapped(aMappedAtCreation)
  , mMappedWritable(aMappedAtCreation)
{}

GPUBuffer::~GPUBuffer()
{
  if (mBuffer) {
    wgpuBufferRelease(mBuffer);
    mBuffer = nullptr;
  }
}

void
GPUBuffer::GetMapState(nsAString& aOut) const
{
  aOut.Assign(mMapped ? NS_LITERAL_STRING("mapped") : NS_LITERAL_STRING("unmapped"));
}

struct AsyncBufferMapState {
  std::atomic<bool> done{false};
  std::atomic<bool> ok{false};
};

static void
OnBufferMapDoneAsync(WGPUMapAsyncStatus aStatus,
                     WGPUStringView aMessage,
                     void* aUserdata1,
                     void* aUserdata2)
{
  auto* st = static_cast<AsyncBufferMapState*>(aUserdata1);
  if (st) {
    st->ok.store(aStatus == WGPUMapAsyncStatus_Success, std::memory_order_release);
    st->done.store(true, std::memory_order_release);
  }
}

class AsyncBufferMapPoller final : public nsITimerCallback
{
public:
  NS_DECL_ISUPPORTS

  AsyncBufferMapPoller(GPUBuffer* aBuf,
                       Promise* aPromise,
                       WGPUDevice aDevice,
                       AsyncBufferMapState* aState,
                       bool aWritable)
    : mBuffer(aBuf)
    , mPromise(aPromise)
    , mDevice(aDevice)
    , mState(aState)
    , mWritable(aWritable)
  {
    wgpuDeviceAddRef(mDevice);
  }

  void Start() {
    mTimer = do_CreateInstance(NS_TIMER_CONTRACTID);
    if (mTimer) {
      mTimer->InitWithCallback(this, 2, nsITimer::TYPE_REPEATING_SLACK);
    } else {
      // Fallback: block if timer unavailable
      wgpuDevicePoll(mDevice, true, nullptr);
      Finish();
    }
  }

  NS_IMETHOD Notify(nsITimer* aTimer) override {
    wgpuDevicePoll(mDevice, false, nullptr);
    if (mState->done.load(std::memory_order_acquire)) {
      if (mTimer) {
        mTimer->Cancel();
        mTimer = nullptr;
      }
      Finish();
    }
    return NS_OK;
  }

private:
  ~AsyncBufferMapPoller() {
    if (mTimer) {
      mTimer->Cancel();
      mTimer = nullptr;
    }
    if (mDevice) {
      wgpuDeviceRelease(mDevice);
      mDevice = nullptr;
    }
    delete mState;
  }

  void Finish() {
    bool ok = mState->ok.load(std::memory_order_acquire);
    if (!ok) {
      mPromise->MaybeReject(NS_ERROR_DOM_OPERATION_ERR);
    } else {
      mBuffer->SetMappedState(true, mWritable);
      mPromise->MaybeResolveWithUndefined();
    }
    mBuffer = nullptr;
    mPromise = nullptr;
  }

  RefPtr<GPUBuffer> mBuffer;
  RefPtr<Promise> mPromise;
  WGPUDevice mDevice;
  AsyncBufferMapState* mState;
  bool mWritable;
  nsCOMPtr<nsITimer> mTimer;
};

NS_IMPL_ISUPPORTS(AsyncBufferMapPoller, nsITimerCallback)

already_AddRefed<Promise>
GPUBuffer::MapAsync(uint32_t aMode, uint64_t aOffset, const Optional<uint64_t>& aSize, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;

  uint64_t sz = aSize.WasPassed() ? aSize.Value() : (mSize - aOffset);
  bool writable = (aMode & 0x0002) != 0;
  auto* st = new AsyncBufferMapState();

  WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  cb.mode = WGPUCallbackMode_AllowSpontaneous;
  cb.callback = OnBufferMapDoneAsync;
  cb.userdata1 = st;

  wgpuBufferMapAsync(mBuffer, static_cast<WGPUMapMode>(aMode), static_cast<size_t>(aOffset), static_cast<size_t>(sz), cb);
  wgpuDevicePoll(mDevice, false, nullptr);

  if (st->done.load(std::memory_order_acquire)) {
    bool ok = st->ok.load(std::memory_order_acquire);
    delete st;
    if (!ok) {
      promise->MaybeReject(NS_ERROR_DOM_OPERATION_ERR);
    } else {
      mMapped = true;
      mMappedWritable = writable;
      promise->MaybeResolveWithUndefined();
    }
    return promise.forget();
  }

  if (NS_IsMainThread()) {
    RefPtr<AsyncBufferMapPoller> poller =
      new AsyncBufferMapPoller(this, promise, mDevice, st, writable);
    poller->Start();
  } else {
    wgpuDevicePoll(mDevice, true, nullptr);
    bool ok = st->ok.load(std::memory_order_acquire);
    delete st;
    if (!ok) {
      promise->MaybeReject(NS_ERROR_DOM_OPERATION_ERR);
    } else {
      mMapped = true;
      mMappedWritable = writable;
      promise->MaybeResolveWithUndefined();
    }
  }
  return promise.forget();
}

void
GPUBuffer::GetMappedRange(JSContext* aCx, uint64_t aOffset, const Optional<uint64_t>& aSize,
                          JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv)
{
  if (!mMapped) {
    aRv.Throw(NS_ERROR_DOM_OPERATION_ERR);
    return;
  }
  uint64_t sz = aSize.WasPassed() ? aSize.Value() : (mSize - aOffset);
  JS::Rooted<JSObject*> ab(aCx, JS_NewArrayBuffer(aCx, static_cast<uint32_t>(sz)));
  if (!ab) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  const void* src = mMappedWritable
    ? wgpuBufferGetMappedRange(mBuffer, static_cast<size_t>(aOffset), static_cast<size_t>(sz))
    : wgpuBufferGetConstMappedRange(mBuffer, static_cast<size_t>(aOffset), static_cast<size_t>(sz));
  if (src && sz > 0) {
    JS::AutoCheckCannotGC nogc;
    bool isShared = false;
    uint8_t* dst = JS_GetArrayBufferData(ab, &isShared, nogc);
    if (dst) {
      memcpy(dst, src, static_cast<size_t>(sz));
    }
  }

  MappedRangeRecord rec;
  rec.mOffset = aOffset;
  rec.mSize = sz;
  rec.mWritable = mMappedWritable;
  rec.mArrayBuffer = ab;
  mMappedRanges.AppendElement(rec);

  aRetval.setObject(*ab);
}

void
GPUBuffer::Unmap()
{
  if (!mMapped) return;
  for (uint32_t i = 0; i < mMappedRanges.Length(); ++i) {
    auto& rec = mMappedRanges[i];
    if (rec.mWritable && rec.mArrayBuffer) {
      void* dst = wgpuBufferGetMappedRange(mBuffer, static_cast<size_t>(rec.mOffset), static_cast<size_t>(rec.mSize));
      if (dst && rec.mSize > 0) {
        JS::AutoCheckCannotGC nogc;
        bool isShared = false;
        uint8_t* src = JS_GetArrayBufferData(rec.mArrayBuffer, &isShared, nogc);
        if (src) {
          memcpy(dst, src, static_cast<size_t>(rec.mSize));
        }
      }
    }
  }
  mMappedRanges.Clear();
  wgpuBufferUnmap(mBuffer);
  mMapped = false;
  mMappedWritable = false;
}

void
GPUBuffer::Destroy()
{
  mMappedRanges.Clear();
  mMapped = false;
  if (mBuffer) {
    wgpuBufferDestroy(mBuffer);
  }
}

NS_IMPL_CYCLE_COLLECTION_CLASS(GPUBuffer)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(GPUBuffer)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mOwner)
  tmp->mMappedRanges.Clear();
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(GPUBuffer)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mOwner)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END
NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(GPUBuffer)
  for (uint32_t i = 0; i < tmp->mMappedRanges.Length(); ++i) {
    NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mMappedRanges[i].mArrayBuffer)
  }
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUBuffer)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUBuffer)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUBuffer)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUBuffer::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUBufferBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUTexture
 ******************************************************************************/

GPUTexture::GPUTexture(nsIGlobalObject* aGlobal, WGPUTexture aTexture,
                       uint32_t aWidth, uint32_t aHeight, uint32_t aDepth,
                       uint32_t aMipLevels, uint32_t aSamples,
                       const nsAString& aDimension, const nsAString& aFormat,
                       uint32_t aUsage, bool aOwned)
  : mOwner(aGlobal), mTexture(aTexture)
  , mWidth(aWidth), mHeight(aHeight), mDepth(aDepth)
  , mMipLevels(aMipLevels), mSamples(aSamples), mUsage(aUsage)
  , mDimension(aDimension), mFormat(aFormat), mOwned(aOwned)
{}

GPUTexture::~GPUTexture()
{
  if (mTexture && mOwned) {
    wgpuTextureRelease(mTexture);
    mTexture = nullptr;
  }
}

already_AddRefed<GPUTextureView>
GPUTexture::CreateView(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
  nsCString label, s;
  if (aDesc.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
    if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
    if (GetPropString(aCx, obj, "format", s)) desc.format = ParseTextureFormat(s);
    if (GetPropString(aCx, obj, "dimension", s)) desc.dimension = ParseTextureViewDimension(s);
    if (GetPropString(aCx, obj, "aspect", s)) desc.aspect = ParseTextureAspect(s);
    GetPropU32(aCx, obj, "baseMipLevel", desc.baseMipLevel);
    GetPropU32(aCx, obj, "mipLevelCount", desc.mipLevelCount);
    GetPropU32(aCx, obj, "baseArrayLayer", desc.baseArrayLayer);
    GetPropU32(aCx, obj, "arrayLayerCount", desc.arrayLayerCount);
  }
  WGPUTextureView raw = wgpuTextureCreateView(mTexture, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUTextureView> ret = new GPUTextureView(mOwner, raw);
  return ret.forget();
}

void
GPUTexture::Destroy()
{
  if (mTexture) {
    wgpuTextureDestroy(mTexture);
  }
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUTexture, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUTexture)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUTexture)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUTexture)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUTexture::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUTextureBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * Simple WGPU wrapper classes
 ******************************************************************************/

#define IMPL_SIMPLE_WGPU_CLASS(ClassName, RawType, ReleaseFn) \
ClassName::ClassName(nsIGlobalObject* aGlobal, RawType aRaw) : mOwner(aGlobal), mRaw(aRaw) {} \
ClassName::~ClassName() { if (mRaw) { ReleaseFn(mRaw); mRaw = nullptr; } } \
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(ClassName, mOwner) \
NS_IMPL_CYCLE_COLLECTING_ADDREF(ClassName) \
NS_IMPL_CYCLE_COLLECTING_RELEASE(ClassName) \
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ClassName) \
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY \
  NS_INTERFACE_MAP_ENTRY(nsISupports) \
NS_INTERFACE_MAP_END \
JSObject* ClassName::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) { \
  return ClassName##Binding::Wrap(aCx, this, aGivenProto); \
}

IMPL_SIMPLE_WGPU_CLASS(GPUTextureView, WGPUTextureView, wgpuTextureViewRelease)
IMPL_SIMPLE_WGPU_CLASS(GPUSampler, WGPUSampler, wgpuSamplerRelease)
IMPL_SIMPLE_WGPU_CLASS(GPUBindGroupLayout, WGPUBindGroupLayout, wgpuBindGroupLayoutRelease)
IMPL_SIMPLE_WGPU_CLASS(GPUPipelineLayout, WGPUPipelineLayout, wgpuPipelineLayoutRelease)
IMPL_SIMPLE_WGPU_CLASS(GPUBindGroup, WGPUBindGroup, wgpuBindGroupRelease)
IMPL_SIMPLE_WGPU_CLASS(GPURenderBundle, WGPURenderBundle, wgpuRenderBundleRelease)
IMPL_SIMPLE_WGPU_CLASS(GPUCommandBuffer, WGPUCommandBuffer, wgpuCommandBufferRelease)

#undef IMPL_SIMPLE_WGPU_CLASS

/*******************************************************************************
 * GPUCompilationInfo / GPUShaderModule / Pipelines / QuerySet
 ******************************************************************************/

GPUCompilationInfo::GPUCompilationInfo(nsIGlobalObject* aGlobal) : mOwner(aGlobal) {}
void GPUCompilationInfo::GetMessages(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval) {
  JSObject* arr = JS_NewArrayObject(aCx, 0);
  if (arr) aRetval.setObject(*arr);
  else aRetval.setUndefined();
}
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUCompilationInfo, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUCompilationInfo)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUCompilationInfo)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUCompilationInfo)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPUCompilationInfo::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPUCompilationInfoBinding::Wrap(aCx, this, aGivenProto);
}

GPUShaderModule::GPUShaderModule(nsIGlobalObject* aGlobal, WGPUShaderModule aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPUShaderModule::~GPUShaderModule() { if (mRaw) { wgpuShaderModuleRelease(mRaw); mRaw = nullptr; } }
already_AddRefed<Promise> GPUShaderModule::GetCompilationInfo(ErrorResult& aRv) {
  RefPtr<Promise> p = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) return nullptr;
  RefPtr<GPUCompilationInfo> info = new GPUCompilationInfo(mOwner);
  p->MaybeResolve(info);
  return p.forget();
}
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUShaderModule, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUShaderModule)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUShaderModule)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUShaderModule)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPUShaderModule::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPUShaderModuleBinding::Wrap(aCx, this, aGivenProto);
}

GPUComputePipeline::GPUComputePipeline(nsIGlobalObject* aGlobal, WGPUComputePipeline aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPUComputePipeline::~GPUComputePipeline() { if (mRaw) { wgpuComputePipelineRelease(mRaw); mRaw = nullptr; } }
already_AddRefed<GPUBindGroupLayout> GPUComputePipeline::GetBindGroupLayout(uint32_t aIndex, ErrorResult& aRv) {
  WGPUBindGroupLayout raw = wgpuComputePipelineGetBindGroupLayout(mRaw, aIndex);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUBindGroupLayout> ret = new GPUBindGroupLayout(mOwner, raw);
  return ret.forget();
}
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUComputePipeline, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUComputePipeline)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUComputePipeline)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUComputePipeline)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPUComputePipeline::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPUComputePipelineBinding::Wrap(aCx, this, aGivenProto);
}

GPURenderPipeline::GPURenderPipeline(nsIGlobalObject* aGlobal, WGPURenderPipeline aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPURenderPipeline::~GPURenderPipeline() { if (mRaw) { wgpuRenderPipelineRelease(mRaw); mRaw = nullptr; } }
already_AddRefed<GPUBindGroupLayout> GPURenderPipeline::GetBindGroupLayout(uint32_t aIndex, ErrorResult& aRv) {
  WGPUBindGroupLayout raw = wgpuRenderPipelineGetBindGroupLayout(mRaw, aIndex);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUBindGroupLayout> ret = new GPUBindGroupLayout(mOwner, raw);
  return ret.forget();
}
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPURenderPipeline, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPURenderPipeline)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPURenderPipeline)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPURenderPipeline)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPURenderPipeline::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPURenderPipelineBinding::Wrap(aCx, this, aGivenProto);
}

GPUQuerySet::GPUQuerySet(nsIGlobalObject* aGlobal, WGPUQuerySet aRaw, const nsAString& aType, uint32_t aCount)
  : mOwner(aGlobal), mRaw(aRaw), mType(aType), mCount(aCount) {}
GPUQuerySet::~GPUQuerySet() { if (mRaw) { wgpuQuerySetRelease(mRaw); mRaw = nullptr; } }
void GPUQuerySet::Destroy() { if (mRaw) wgpuQuerySetDestroy(mRaw); }
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUQuerySet, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUQuerySet)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUQuerySet)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUQuerySet)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPUQuerySet::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPUQuerySetBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUCommandEncoder
 ******************************************************************************/

GPUCommandEncoder::GPUCommandEncoder(nsIGlobalObject* aGlobal, WGPUCommandEncoder aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPUCommandEncoder::~GPUCommandEncoder() { if (mRaw) { wgpuCommandEncoderRelease(mRaw); mRaw = nullptr; } }

already_AddRefed<GPURenderPassEncoder>
GPUCommandEncoder::BeginRenderPass(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  if (!aDesc.isObject()) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return nullptr; }
  JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
  WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  nsCString label, s;
  if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);

  std::vector<WGPURenderPassColorAttachment> colorAtts;
  JS::Rooted<JSObject*> caObj(aCx);
  if (GetPropObj(aCx, obj, "colorAttachments", &caObj)) {
    uint32_t len = 0;
    if (JS_GetArrayLength(aCx, caObj, &len)) {
      colorAtts.resize(len, WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT);
      for (uint32_t i = 0; i < len; ++i) {
        JS::Rooted<JS::Value> av(aCx);
        if (!JS_GetElement(aCx, caObj, i, &av) || !av.isObject()) continue;
        JS::Rooted<JSObject*> ao(aCx, &av.toObject());
        JS::Rooted<JSObject*> viewObj(aCx);
        if (GetPropObj(aCx, ao, "view", &viewObj)) {
          RefPtr<GPUTextureView> tv;
          if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUTextureView, viewObj, tv)) && tv) {
            colorAtts[i].view = tv->Raw();
          }
        }
        JS::Rooted<JSObject*> rtObj(aCx);
        if (GetPropObj(aCx, ao, "resolveTarget", &rtObj)) {
          RefPtr<GPUTextureView> tv;
          if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUTextureView, rtObj, tv)) && tv) {
            colorAtts[i].resolveTarget = tv->Raw();
          }
        }
        if (GetPropString(aCx, ao, "loadOp", s)) colorAtts[i].loadOp = ParseLoadOp(s);
        if (GetPropString(aCx, ao, "storeOp", s)) colorAtts[i].storeOp = ParseStoreOp(s);
        JS::Rooted<JS::Value> cv(aCx);
        if (GetPropVal(aCx, ao, "clearValue", &cv)) {
          colorAtts[i].clearValue = ParseColor(aCx, cv);
        }
      }
      desc.colorAttachmentCount = colorAtts.size();
      desc.colorAttachments = colorAtts.data();
    }
  }

  WGPURenderPassDepthStencilAttachment dsAtt = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  JS::Rooted<JSObject*> dsObj(aCx);
  if (GetPropObj(aCx, obj, "depthStencilAttachment", &dsObj)) {
    JS::Rooted<JSObject*> viewObj(aCx);
    if (GetPropObj(aCx, dsObj, "view", &viewObj)) {
      RefPtr<GPUTextureView> tv;
      if (NS_SUCCEEDED(UNWRAP_OBJECT(GPUTextureView, viewObj, tv)) && tv) {
        dsAtt.view = tv->Raw();
      }
    }
    if (GetPropString(aCx, dsObj, "depthLoadOp", s)) dsAtt.depthLoadOp = ParseLoadOp(s);
    if (GetPropString(aCx, dsObj, "depthStoreOp", s)) dsAtt.depthStoreOp = ParseStoreOp(s);
    GetPropFloat(aCx, dsObj, "depthClearValue", dsAtt.depthClearValue);
    bool dro = false, sro = false;
    if (GetPropBool(aCx, dsObj, "depthReadOnly", dro)) dsAtt.depthReadOnly = dro;
    if (GetPropString(aCx, dsObj, "stencilLoadOp", s)) dsAtt.stencilLoadOp = ParseLoadOp(s);
    if (GetPropString(aCx, dsObj, "stencilStoreOp", s)) dsAtt.stencilStoreOp = ParseStoreOp(s);
    GetPropU32(aCx, dsObj, "stencilClearValue", dsAtt.stencilClearValue);
    if (GetPropBool(aCx, dsObj, "stencilReadOnly", sro)) dsAtt.stencilReadOnly = sro;
    desc.depthStencilAttachment = &dsAtt;
  }

  WGPURenderPassEncoder raw = wgpuCommandEncoderBeginRenderPass(mRaw, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPURenderPassEncoder> ret = new GPURenderPassEncoder(mOwner, raw);
  return ret.forget();
}

already_AddRefed<GPUComputePassEncoder>
GPUCommandEncoder::BeginComputePass(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv)
{
  WGPUComputePassDescriptor desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
  nsCString label;
  if (aDesc.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
    if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  }
  WGPUComputePassEncoder raw = wgpuCommandEncoderBeginComputePass(mRaw, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUComputePassEncoder> ret = new GPUComputePassEncoder(mOwner, raw);
  return ret.forget();
}

void GPUCommandEncoder::CopyBufferToBuffer(GPUBuffer& aSrc, uint64_t aSrcOff, GPUBuffer& aDst, uint64_t aDstOff, uint64_t aSize) {
  wgpuCommandEncoderCopyBufferToBuffer(mRaw, aSrc.Raw(), aSrcOff, aDst.Raw(), aDstOff, aSize);
}
void GPUCommandEncoder::CopyBufferToTexture(JSContext* aCx, JS::Handle<JS::Value> aSrc, JS::Handle<JS::Value> aDst, JS::Handle<JS::Value> aSize, ErrorResult& aRv) {
  WGPUTexelCopyBufferInfo src; WGPUTexelCopyTextureInfo dst;
  if (!ParseTexelCopyBufferInfo(aCx, aSrc, src) || !ParseTexelCopyTextureInfo(aCx, aDst, dst)) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return; }
  WGPUExtent3D ext = ParseExtent3D(aCx, aSize);
  wgpuCommandEncoderCopyBufferToTexture(mRaw, &src, &dst, &ext);
}
void GPUCommandEncoder::CopyTextureToBuffer(JSContext* aCx, JS::Handle<JS::Value> aSrc, JS::Handle<JS::Value> aDst, JS::Handle<JS::Value> aSize, ErrorResult& aRv) {
  WGPUTexelCopyTextureInfo src; WGPUTexelCopyBufferInfo dst;
  if (!ParseTexelCopyTextureInfo(aCx, aSrc, src) || !ParseTexelCopyBufferInfo(aCx, aDst, dst)) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return; }
  WGPUExtent3D ext = ParseExtent3D(aCx, aSize);
  wgpuCommandEncoderCopyTextureToBuffer(mRaw, &src, &dst, &ext);
}
void GPUCommandEncoder::CopyTextureToTexture(JSContext* aCx, JS::Handle<JS::Value> aSrc, JS::Handle<JS::Value> aDst, JS::Handle<JS::Value> aSize, ErrorResult& aRv) {
  WGPUTexelCopyTextureInfo src, dst;
  if (!ParseTexelCopyTextureInfo(aCx, aSrc, src) || !ParseTexelCopyTextureInfo(aCx, aDst, dst)) { aRv.Throw(NS_ERROR_DOM_TYPE_ERR); return; }
  WGPUExtent3D ext = ParseExtent3D(aCx, aSize);
  wgpuCommandEncoderCopyTextureToTexture(mRaw, &src, &dst, &ext);
}
void GPUCommandEncoder::ClearBuffer(GPUBuffer& aBuffer, uint64_t aOffset, const Optional<uint64_t>& aSize) {
  uint64_t sz = aSize.WasPassed() ? aSize.Value() : WGPU_WHOLE_SIZE;
  wgpuCommandEncoderClearBuffer(mRaw, aBuffer.Raw(), aOffset, sz);
}
void GPUCommandEncoder::ResolveQuerySet(GPUQuerySet& aQuerySet, uint32_t aFirstQuery, uint32_t aQueryCount, GPUBuffer& aDst, uint64_t aDstOff) {
  wgpuCommandEncoderResolveQuerySet(mRaw, aQuerySet.Raw(), aFirstQuery, aQueryCount, aDst.Raw(), aDstOff);
}
void GPUCommandEncoder::PushDebugGroup(const nsAString& aLabel) {
  nsCString s = NS_ConvertUTF16toUTF8(aLabel);
  wgpuCommandEncoderPushDebugGroup(mRaw, ToWGPUString(s));
}
void GPUCommandEncoder::PopDebugGroup() { wgpuCommandEncoderPopDebugGroup(mRaw); }
void GPUCommandEncoder::InsertDebugMarker(const nsAString& aLabel) {
  nsCString s = NS_ConvertUTF16toUTF8(aLabel);
  wgpuCommandEncoderInsertDebugMarker(mRaw, ToWGPUString(s));
}
already_AddRefed<GPUCommandBuffer> GPUCommandEncoder::Finish(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv) {
  WGPUCommandBufferDescriptor desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
  nsCString label;
  if (aDesc.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
    if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  }
  WGPUCommandBuffer raw = wgpuCommandEncoderFinish(mRaw, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPUCommandBuffer> ret = new GPUCommandBuffer(mOwner, raw);
  return ret.forget();
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUCommandEncoder, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUCommandEncoder)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUCommandEncoder)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUCommandEncoder)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPUCommandEncoder::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPUCommandEncoderBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPURenderPassEncoder
 ******************************************************************************/

GPURenderPassEncoder::GPURenderPassEncoder(nsIGlobalObject* aGlobal, WGPURenderPassEncoder aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPURenderPassEncoder::~GPURenderPassEncoder() { if (mRaw) { wgpuRenderPassEncoderRelease(mRaw); mRaw = nullptr; } }

void GPURenderPassEncoder::SetPipeline(GPURenderPipeline& aPipeline) { wgpuRenderPassEncoderSetPipeline(mRaw, aPipeline.Raw()); }
void GPURenderPassEncoder::SetBindGroup(uint32_t aIndex, GPUBindGroup* aBindGroup, const Optional<Sequence<uint32_t>>& aDynamicOffsets) {
  size_t count = aDynamicOffsets.WasPassed() ? aDynamicOffsets.Value().Length() : 0;
  const uint32_t* ptr = count > 0 ? aDynamicOffsets.Value().Elements() : nullptr;
  wgpuRenderPassEncoderSetBindGroup(mRaw, aIndex, aBindGroup ? aBindGroup->Raw() : nullptr, count, ptr);
}
void GPURenderPassEncoder::SetVertexBuffer(uint32_t aSlot, GPUBuffer* aBuffer, uint64_t aOffset, const Optional<uint64_t>& aSize) {
  uint64_t sz = aSize.WasPassed() ? aSize.Value() : WGPU_WHOLE_SIZE;
  wgpuRenderPassEncoderSetVertexBuffer(mRaw, aSlot, aBuffer ? aBuffer->Raw() : nullptr, aOffset, sz);
}
void GPURenderPassEncoder::SetIndexBuffer(GPUBuffer& aBuffer, const nsAString& aIndexFormat, uint64_t aOffset, const Optional<uint64_t>& aSize) {
  uint64_t sz = aSize.WasPassed() ? aSize.Value() : WGPU_WHOLE_SIZE;
  WGPUIndexFormat fmt = ParseIndexFormat(NS_ConvertUTF16toUTF8(aIndexFormat));
  wgpuRenderPassEncoderSetIndexBuffer(mRaw, aBuffer.Raw(), fmt, aOffset, sz);
}
void GPURenderPassEncoder::Draw(uint32_t aVertexCount, uint32_t aInstanceCount, uint32_t aFirstVertex, uint32_t aFirstInstance) {
  wgpuRenderPassEncoderDraw(mRaw, aVertexCount, aInstanceCount, aFirstVertex, aFirstInstance);
}
void GPURenderPassEncoder::DrawIndexed(uint32_t aIndexCount, uint32_t aInstanceCount, uint32_t aFirstIndex, int32_t aBaseVertex, uint32_t aFirstInstance) {
  wgpuRenderPassEncoderDrawIndexed(mRaw, aIndexCount, aInstanceCount, aFirstIndex, aBaseVertex, aFirstInstance);
}
void GPURenderPassEncoder::DrawIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset) {
  wgpuRenderPassEncoderDrawIndirect(mRaw, aIndirectBuffer.Raw(), aIndirectOffset);
}
void GPURenderPassEncoder::DrawIndexedIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset) {
  wgpuRenderPassEncoderDrawIndexedIndirect(mRaw, aIndirectBuffer.Raw(), aIndirectOffset);
}
void GPURenderPassEncoder::SetViewport(float aX, float aY, float aW, float aH, float aMinD, float aMaxD) {
  wgpuRenderPassEncoderSetViewport(mRaw, aX, aY, aW, aH, aMinD, aMaxD);
}
void GPURenderPassEncoder::SetScissorRect(uint32_t aX, uint32_t aY, uint32_t aW, uint32_t aH) {
  wgpuRenderPassEncoderSetScissorRect(mRaw, aX, aY, aW, aH);
}
void GPURenderPassEncoder::SetBlendConstant(JSContext* aCx, JS::Handle<JS::Value> aColor, ErrorResult& aRv) {
  WGPUColor c = ParseColor(aCx, aColor);
  wgpuRenderPassEncoderSetBlendConstant(mRaw, &c);
}
void GPURenderPassEncoder::SetStencilReference(uint32_t aRef) { wgpuRenderPassEncoderSetStencilReference(mRaw, aRef); }
void GPURenderPassEncoder::ExecuteBundles(const Sequence<OwningNonNull<GPURenderBundle>>& aBundles) {
  std::vector<WGPURenderBundle> raw;
  raw.reserve(aBundles.Length());
  for (uint32_t i = 0; i < aBundles.Length(); ++i) raw.push_back(aBundles[i]->Raw());
  wgpuRenderPassEncoderExecuteBundles(mRaw, raw.size(), raw.empty() ? nullptr : raw.data());
}
void GPURenderPassEncoder::BeginOcclusionQuery(uint32_t aQueryIndex) { wgpuRenderPassEncoderBeginOcclusionQuery(mRaw, aQueryIndex); }
void GPURenderPassEncoder::EndOcclusionQuery() { wgpuRenderPassEncoderEndOcclusionQuery(mRaw); }
void GPURenderPassEncoder::PushDebugGroup(const nsAString& aLabel) {
  nsCString s = NS_ConvertUTF16toUTF8(aLabel);
  wgpuRenderPassEncoderPushDebugGroup(mRaw, ToWGPUString(s));
}
void GPURenderPassEncoder::PopDebugGroup() { wgpuRenderPassEncoderPopDebugGroup(mRaw); }
void GPURenderPassEncoder::InsertDebugMarker(const nsAString& aLabel) {
  nsCString s = NS_ConvertUTF16toUTF8(aLabel);
  wgpuRenderPassEncoderInsertDebugMarker(mRaw, ToWGPUString(s));
}
void GPURenderPassEncoder::End() { wgpuRenderPassEncoderEnd(mRaw); }

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPURenderPassEncoder, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPURenderPassEncoder)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPURenderPassEncoder)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPURenderPassEncoder)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPURenderPassEncoder::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPURenderPassEncoderBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPUComputePassEncoder
 ******************************************************************************/

GPUComputePassEncoder::GPUComputePassEncoder(nsIGlobalObject* aGlobal, WGPUComputePassEncoder aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPUComputePassEncoder::~GPUComputePassEncoder() { if (mRaw) { wgpuComputePassEncoderRelease(mRaw); mRaw = nullptr; } }

void GPUComputePassEncoder::SetPipeline(GPUComputePipeline& aPipeline) { wgpuComputePassEncoderSetPipeline(mRaw, aPipeline.Raw()); }
void GPUComputePassEncoder::SetBindGroup(uint32_t aIndex, GPUBindGroup* aBindGroup, const Optional<Sequence<uint32_t>>& aDynamicOffsets) {
  size_t count = aDynamicOffsets.WasPassed() ? aDynamicOffsets.Value().Length() : 0;
  const uint32_t* ptr = count > 0 ? aDynamicOffsets.Value().Elements() : nullptr;
  wgpuComputePassEncoderSetBindGroup(mRaw, aIndex, aBindGroup ? aBindGroup->Raw() : nullptr, count, ptr);
}
void GPUComputePassEncoder::DispatchWorkgroups(uint32_t aX, uint32_t aY, uint32_t aZ) {
  wgpuComputePassEncoderDispatchWorkgroups(mRaw, aX, aY, aZ);
}
void GPUComputePassEncoder::DispatchWorkgroupsIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset) {
  wgpuComputePassEncoderDispatchWorkgroupsIndirect(mRaw, aIndirectBuffer.Raw(), aIndirectOffset);
}
void GPUComputePassEncoder::PushDebugGroup(const nsAString& aLabel) {
  nsCString s = NS_ConvertUTF16toUTF8(aLabel);
  wgpuComputePassEncoderPushDebugGroup(mRaw, ToWGPUString(s));
}
void GPUComputePassEncoder::PopDebugGroup() { wgpuComputePassEncoderPopDebugGroup(mRaw); }
void GPUComputePassEncoder::InsertDebugMarker(const nsAString& aLabel) {
  nsCString s = NS_ConvertUTF16toUTF8(aLabel);
  wgpuComputePassEncoderInsertDebugMarker(mRaw, ToWGPUString(s));
}
void GPUComputePassEncoder::End() { wgpuComputePassEncoderEnd(mRaw); }

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUComputePassEncoder, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUComputePassEncoder)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUComputePassEncoder)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUComputePassEncoder)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPUComputePassEncoder::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPUComputePassEncoderBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * GPURenderBundleEncoder
 ******************************************************************************/

GPURenderBundleEncoder::GPURenderBundleEncoder(nsIGlobalObject* aGlobal, WGPURenderBundleEncoder aRaw) : mOwner(aGlobal), mRaw(aRaw) {}
GPURenderBundleEncoder::~GPURenderBundleEncoder() { if (mRaw) { wgpuRenderBundleEncoderRelease(mRaw); mRaw = nullptr; } }

void GPURenderBundleEncoder::SetPipeline(GPURenderPipeline& aPipeline) { wgpuRenderBundleEncoderSetPipeline(mRaw, aPipeline.Raw()); }
void GPURenderBundleEncoder::SetBindGroup(uint32_t aIndex, GPUBindGroup* aBindGroup, const Optional<Sequence<uint32_t>>& aDynamicOffsets) {
  size_t count = aDynamicOffsets.WasPassed() ? aDynamicOffsets.Value().Length() : 0;
  const uint32_t* ptr = count > 0 ? aDynamicOffsets.Value().Elements() : nullptr;
  wgpuRenderBundleEncoderSetBindGroup(mRaw, aIndex, aBindGroup ? aBindGroup->Raw() : nullptr, count, ptr);
}
void GPURenderBundleEncoder::SetVertexBuffer(uint32_t aSlot, GPUBuffer* aBuffer, uint64_t aOffset, const Optional<uint64_t>& aSize) {
  uint64_t sz = aSize.WasPassed() ? aSize.Value() : WGPU_WHOLE_SIZE;
  wgpuRenderBundleEncoderSetVertexBuffer(mRaw, aSlot, aBuffer ? aBuffer->Raw() : nullptr, aOffset, sz);
}
void GPURenderBundleEncoder::SetIndexBuffer(GPUBuffer& aBuffer, const nsAString& aIndexFormat, uint64_t aOffset, const Optional<uint64_t>& aSize) {
  uint64_t sz = aSize.WasPassed() ? aSize.Value() : WGPU_WHOLE_SIZE;
  WGPUIndexFormat fmt = ParseIndexFormat(NS_ConvertUTF16toUTF8(aIndexFormat));
  wgpuRenderBundleEncoderSetIndexBuffer(mRaw, aBuffer.Raw(), fmt, aOffset, sz);
}
void GPURenderBundleEncoder::Draw(uint32_t aVertexCount, uint32_t aInstanceCount, uint32_t aFirstVertex, uint32_t aFirstInstance) {
  wgpuRenderBundleEncoderDraw(mRaw, aVertexCount, aInstanceCount, aFirstVertex, aFirstInstance);
}
void GPURenderBundleEncoder::DrawIndexed(uint32_t aIndexCount, uint32_t aInstanceCount, uint32_t aFirstIndex, int32_t aBaseVertex, uint32_t aFirstInstance) {
  wgpuRenderBundleEncoderDrawIndexed(mRaw, aIndexCount, aInstanceCount, aFirstIndex, aBaseVertex, aFirstInstance);
}
void GPURenderBundleEncoder::DrawIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset) {
  wgpuRenderBundleEncoderDrawIndirect(mRaw, aIndirectBuffer.Raw(), aIndirectOffset);
}
void GPURenderBundleEncoder::DrawIndexedIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset) {
  wgpuRenderBundleEncoderDrawIndexedIndirect(mRaw, aIndirectBuffer.Raw(), aIndirectOffset);
}
already_AddRefed<GPURenderBundle> GPURenderBundleEncoder::Finish(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv) {
  WGPURenderBundleDescriptor desc = WGPU_RENDER_BUNDLE_DESCRIPTOR_INIT;
  nsCString label;
  if (aDesc.isObject()) {
    JS::Rooted<JSObject*> obj(aCx, &aDesc.toObject());
    if (GetPropString(aCx, obj, "label", label)) desc.label = ToWGPUString(label);
  }
  WGPURenderBundle raw = wgpuRenderBundleEncoderFinish(mRaw, &desc);
  if (!raw) { aRv.Throw(NS_ERROR_DOM_OPERATION_ERR); return nullptr; }
  RefPtr<GPURenderBundle> ret = new GPURenderBundle(mOwner, raw);
  return ret.forget();
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPURenderBundleEncoder, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(GPURenderBundleEncoder)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPURenderBundleEncoder)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPURenderBundleEncoder)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END
JSObject* GPURenderBundleEncoder::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return GPURenderBundleEncoderBinding::Wrap(aCx, this, aGivenProto);
}

} // namespace dom
} // namespace mozilla
