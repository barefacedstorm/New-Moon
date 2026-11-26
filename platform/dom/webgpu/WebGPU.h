/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WebGPU_h
#define mozilla_dom_WebGPU_h

#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/TypedArray.h"
#include "mozilla/dom/UnionTypes.h"
#include "mozilla/gfx/2D.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsICanvasRenderingContextInternal.h"
#include "nsIGlobalObject.h"
#include "nsISupports.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"

#pragma GCC visibility push(default)
#include "webgpu/webgpu.h"
#include "webgpu/wgpu.h"
#pragma GCC visibility pop

namespace mozilla {
namespace layers {
class Image;
class ImageContainer;
}
namespace dom {

class Promise;
class HTMLCanvasElement;
class GPU;
class GPUAdapter;
class GPUAdapterInfo;
class GPUSupportedLimits;
class GPUDevice;
class GPUDeviceLostInfo;
class GPUError;
class GPUQueue;
class GPUBuffer;
class GPUTexture;
class GPUTextureView;
class GPUSampler;
class GPUBindGroupLayout;
class GPUPipelineLayout;
class GPUBindGroup;
class GPUShaderModule;
class GPUCompilationInfo;
class GPUComputePipeline;
class GPURenderPipeline;
class GPUCommandBuffer;
class GPUCommandEncoder;
class GPURenderPassEncoder;
class GPUComputePassEncoder;
class GPURenderBundle;
class GPURenderBundleEncoder;
class GPUQuerySet;
class GPUCanvasContext;

// Register/unregister active GPUCanvasContexts so GPUQueue::Submit presents them.
void RegisterWebGPUCanvasContext(GPUCanvasContext* aCtx);
void UnregisterWebGPUCanvasContext(GPUCanvasContext* aCtx);
void PresentAllWebGPUCanvasContextsForDevice(WGPUDevice aDevice);

class GPU final
  : public nsISupports
  , public nsWrapperCache
{
public:
  explicit GPU(nsIGlobalObject* aGlobal);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUInstance Raw() const { return mInstance; }

  already_AddRefed<Promise> RequestAdapter(JSContext* aCx, JS::Handle<JS::Value> aOptions, ErrorResult& aRv);
  void GetPreferredCanvasFormat(nsAString& aRetval) const;
  void GetWgslLanguageFeatures(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPU)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPU();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUInstance mInstance;
};

class GPUAdapterInfo final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUAdapterInfo(nsIGlobalObject* aGlobal,
                 const nsAString& aVendor,
                 const nsAString& aArch,
                 const nsAString& aDevice,
                 const nsAString& aDesc);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetVendor(nsAString& aOut) const { aOut = mVendor; }
  void GetArchitecture(nsAString& aOut) const { aOut = mArchitecture; }
  void GetDevice(nsAString& aOut) const { aOut = mDevice; }
  void GetDescription(nsAString& aOut) const { aOut = mDescription; }

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUAdapterInfo)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUAdapterInfo() = default;
  nsCOMPtr<nsIGlobalObject> mOwner;
  nsString mVendor;
  nsString mArchitecture;
  nsString mDevice;
  nsString mDescription;
};

class GPUSupportedLimits final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUSupportedLimits(nsIGlobalObject* aGlobal, const WGPULimits& aLimits);

  nsIGlobalObject* GetParentObject() const { return mOwner; }

  uint32_t MaxTextureDimension1D() const { return mLimits.maxTextureDimension1D; }
  uint32_t MaxTextureDimension2D() const { return mLimits.maxTextureDimension2D; }
  uint32_t MaxTextureDimension3D() const { return mLimits.maxTextureDimension3D; }
  uint32_t MaxTextureArrayLayers() const { return mLimits.maxTextureArrayLayers; }
  uint32_t MaxBindGroups() const { return mLimits.maxBindGroups; }
  uint32_t MaxBindGroupsPlusVertexBuffers() const { return mLimits.maxBindGroupsPlusVertexBuffers; }
  uint32_t MaxBindingsPerBindGroup() const { return mLimits.maxBindingsPerBindGroup; }
  uint32_t MaxDynamicUniformBuffersPerPipelineLayout() const { return mLimits.maxDynamicUniformBuffersPerPipelineLayout; }
  uint32_t MaxDynamicStorageBuffersPerPipelineLayout() const { return mLimits.maxDynamicStorageBuffersPerPipelineLayout; }
  uint32_t MaxSampledTexturesPerShaderStage() const { return mLimits.maxSampledTexturesPerShaderStage; }
  uint32_t MaxSamplersPerShaderStage() const { return mLimits.maxSamplersPerShaderStage; }
  uint32_t MaxStorageBuffersPerShaderStage() const { return mLimits.maxStorageBuffersPerShaderStage; }
  uint32_t MaxStorageTexturesPerShaderStage() const { return mLimits.maxStorageTexturesPerShaderStage; }
  uint32_t MaxUniformBuffersPerShaderStage() const { return mLimits.maxUniformBuffersPerShaderStage; }
  uint64_t MaxUniformBufferBindingSize() const { return mLimits.maxUniformBufferBindingSize; }
  uint64_t MaxStorageBufferBindingSize() const { return mLimits.maxStorageBufferBindingSize; }
  uint32_t MinUniformBufferOffsetAlignment() const { return mLimits.minUniformBufferOffsetAlignment; }
  uint32_t MinStorageBufferOffsetAlignment() const { return mLimits.minStorageBufferOffsetAlignment; }
  uint32_t MaxVertexBuffers() const { return mLimits.maxVertexBuffers; }
  uint64_t MaxBufferSize() const { return mLimits.maxBufferSize; }
  uint32_t MaxVertexAttributes() const { return mLimits.maxVertexAttributes; }
  uint32_t MaxVertexBufferArrayStride() const { return mLimits.maxVertexBufferArrayStride; }
  uint32_t MaxInterStageShaderComponents() const { return mLimits.maxInterStageShaderVariables * 4; }
  uint32_t MaxInterStageShaderVariables() const { return mLimits.maxInterStageShaderVariables; }
  uint32_t MaxColorAttachments() const { return mLimits.maxColorAttachments; }
  uint32_t MaxColorAttachmentBytesPerSample() const { return mLimits.maxColorAttachmentBytesPerSample; }
  uint32_t MaxComputeWorkgroupStorageSize() const { return mLimits.maxComputeWorkgroupStorageSize; }
  uint32_t MaxComputeInvocationsPerWorkgroup() const { return mLimits.maxComputeInvocationsPerWorkgroup; }
  uint32_t MaxComputeWorkgroupSizeX() const { return mLimits.maxComputeWorkgroupSizeX; }
  uint32_t MaxComputeWorkgroupSizeY() const { return mLimits.maxComputeWorkgroupSizeY; }
  uint32_t MaxComputeWorkgroupSizeZ() const { return mLimits.maxComputeWorkgroupSizeZ; }
  uint32_t MaxComputeWorkgroupsPerDimension() const { return mLimits.maxComputeWorkgroupsPerDimension; }

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUSupportedLimits)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUSupportedLimits() = default;
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPULimits mLimits;
};

class GPUAdapter final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUAdapter(nsIGlobalObject* aGlobal, WGPUAdapter aAdapter);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetFeatures(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval);
  GPUSupportedLimits* Limits();
  GPUAdapterInfo* Info();
  bool IsFallbackAdapter() const { return mIsFallback; }

  already_AddRefed<Promise> RequestDevice(JSContext* aCx, JS::Handle<JS::Value> aDescriptor, ErrorResult& aRv);
  already_AddRefed<Promise> RequestAdapterInfo(ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUAdapter)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUAdapter();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUAdapter mAdapter;
  RefPtr<GPUSupportedLimits> mLimits;
  RefPtr<GPUAdapterInfo> mInfo;
  bool mIsFallback;
};

class GPUDeviceLostInfo final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUDeviceLostInfo(nsIGlobalObject* aGlobal, const nsAString& aReason, const nsAString& aMessage);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetReason(nsAString& aOut) const { aOut = mReason; }
  void GetMessage(nsAString& aOut) const { aOut = mMessage; }

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUDeviceLostInfo)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUDeviceLostInfo() = default;
  nsCOMPtr<nsIGlobalObject> mOwner;
  nsString mReason;
  nsString mMessage;
};

class GPUError final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUError(nsIGlobalObject* aGlobal, const nsAString& aMessage);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetMessage(nsAString& aOut) const { aOut = mMessage; }

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUError)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUError() = default;
  nsCOMPtr<nsIGlobalObject> mOwner;
  nsString mMessage;
};

class GPUDevice final
  : public DOMEventTargetHelper
{
public:
  GPUDevice(nsIGlobalObject* aGlobal, WGPUDevice aDevice);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUDevice Raw() const { return mDevice; }

  void GetFeatures(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval);
  GPUSupportedLimits* Limits();
  GPUQueue* Queue();
  already_AddRefed<Promise> GetLost(ErrorResult& aRv);
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }
  EventHandlerNonNull* GetOnuncapturederror() {
    return GetEventHandler(nullptr, NS_LITERAL_STRING("uncapturederror"));
  }
  void SetOnuncapturederror(EventHandlerNonNull* aCallback) {
    SetEventHandler(nullptr, NS_LITERAL_STRING("uncapturederror"), aCallback);
  }

  void Destroy();

  already_AddRefed<GPUBuffer> CreateBuffer(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUTexture> CreateTexture(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUSampler> CreateSampler(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUBindGroupLayout> CreateBindGroupLayout(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUPipelineLayout> CreatePipelineLayout(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUBindGroup> CreateBindGroup(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUShaderModule> CreateShaderModule(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUComputePipeline> CreateComputePipeline(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<Promise> CreateComputePipelineAsync(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPURenderPipeline> CreateRenderPipeline(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<Promise> CreateRenderPipelineAsync(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUCommandEncoder> CreateCommandEncoder(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPURenderBundleEncoder> CreateRenderBundleEncoder(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUQuerySet> CreateQuerySet(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);

  void PushErrorScope(const nsAString& aFilter);
  already_AddRefed<Promise> PopErrorScope(ErrorResult& aRv);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(GPUDevice, DOMEventTargetHelper)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUDevice();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUDevice mDevice;
  RefPtr<GPUSupportedLimits> mLimits;
  RefPtr<GPUQueue> mQueue;
  RefPtr<Promise> mLostPromise;
  nsString mLabel;
};

class GPUQueue final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUQueue(nsIGlobalObject* aGlobal, WGPUDevice aDevice, WGPUQueue aQueue);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUQueue Raw() const { return mQueue; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  void Submit(const Sequence<OwningNonNull<GPUCommandBuffer>>& aCommandBuffers);
  already_AddRefed<Promise> OnSubmittedWorkDone(ErrorResult& aRv);
  void WriteBuffer(JSContext* aCx,
                   GPUBuffer& aBuffer,
                   uint64_t aBufferOffset,
                   JS::Handle<JS::Value> aData,
                   uint64_t aDataOffset,
                   const Optional<uint64_t>& aSize,
                   ErrorResult& aRv);
  void WriteTexture(JSContext* aCx,
                    JS::Handle<JS::Value> aDestination,
                    JS::Handle<JS::Value> aData,
                    JS::Handle<JS::Value> aDataLayout,
                    JS::Handle<JS::Value> aSize,
                    ErrorResult& aRv);
  void CopyExternalImageToTexture(JSContext* aCx,
                                  JS::Handle<JS::Value> aSource,
                                  JS::Handle<JS::Value> aDestination,
                                  JS::Handle<JS::Value> aCopySize,
                                  ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUQueue)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUQueue();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUDevice mDevice;
  WGPUQueue mQueue;
  nsString mLabel;
};

class GPUBuffer final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUBuffer(nsIGlobalObject* aGlobal, WGPUDevice aDevice, WGPUBuffer aBuffer,
            uint64_t aSize, uint32_t aUsage, bool aMappedAtCreation);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUBuffer Raw() const { return mBuffer; }
  uint64_t Size() const { return mSize; }
  uint32_t Usage() const { return mUsage; }
  void GetMapState(nsAString& aOut) const;
  void SetMappedState(bool aMapped, bool aWritable) { mMapped = aMapped; mMappedWritable = aWritable; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  already_AddRefed<Promise> MapAsync(uint32_t aMode, uint64_t aOffset, const Optional<uint64_t>& aSize, ErrorResult& aRv);
  void GetMappedRange(JSContext* aCx, uint64_t aOffset, const Optional<uint64_t>& aSize,
                      JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv);
  void Unmap();
  void Destroy();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUBuffer)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUBuffer();
  struct MappedRangeRecord {
    uint64_t mOffset;
    uint64_t mSize;
    bool mWritable;
    JS::Heap<JSObject*> mArrayBuffer;
  };
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUDevice mDevice;
  WGPUBuffer mBuffer;
  uint64_t mSize;
  uint32_t mUsage;
  bool mMapped;
  bool mMappedWritable;
  nsTArray<MappedRangeRecord> mMappedRanges;
  nsString mLabel;
};

class GPUTexture final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUTexture(nsIGlobalObject* aGlobal, WGPUTexture aTexture,
             uint32_t aWidth, uint32_t aHeight, uint32_t aDepth,
             uint32_t aMipLevels, uint32_t aSamples,
             const nsAString& aDimension, const nsAString& aFormat,
             uint32_t aUsage, bool aOwned = true);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUTexture Raw() const { return mTexture; }
  uint32_t Width() const { return mWidth; }
  uint32_t Height() const { return mHeight; }
  uint32_t DepthOrArrayLayers() const { return mDepth; }
  uint32_t MipLevelCount() const { return mMipLevels; }
  uint32_t SampleCount() const { return mSamples; }
  void GetDimension(nsAString& aOut) const { aOut = mDimension; }
  void GetFormat(nsAString& aOut) const { aOut = mFormat; }
  uint32_t Usage() const { return mUsage; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  already_AddRefed<GPUTextureView> CreateView(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  void Destroy();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUTexture)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUTexture();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUTexture mTexture;
  uint32_t mWidth, mHeight, mDepth, mMipLevels, mSamples, mUsage;
  nsString mDimension, mFormat, mLabel;
  bool mOwned;
};

#define DECL_SIMPLE_WGPU_CLASS(ClassName, RawType) \
class ClassName final : public nsISupports, public nsWrapperCache { \
public: \
  ClassName(nsIGlobalObject* aGlobal, RawType aRaw); \
  nsIGlobalObject* GetParentObject() const { return mOwner; } \
  RawType Raw() const { return mRaw; } \
  void GetLabel(nsAString& aOut) const { aOut = mLabel; } \
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; } \
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS \
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(ClassName) \
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override; \
private: \
  ~ClassName(); \
  nsCOMPtr<nsIGlobalObject> mOwner; \
  RawType mRaw; \
  nsString mLabel; \
};

DECL_SIMPLE_WGPU_CLASS(GPUTextureView, WGPUTextureView)
DECL_SIMPLE_WGPU_CLASS(GPUSampler, WGPUSampler)
DECL_SIMPLE_WGPU_CLASS(GPUBindGroupLayout, WGPUBindGroupLayout)
DECL_SIMPLE_WGPU_CLASS(GPUPipelineLayout, WGPUPipelineLayout)
DECL_SIMPLE_WGPU_CLASS(GPUBindGroup, WGPUBindGroup)
DECL_SIMPLE_WGPU_CLASS(GPURenderBundle, WGPURenderBundle)
DECL_SIMPLE_WGPU_CLASS(GPUCommandBuffer, WGPUCommandBuffer)

#undef DECL_SIMPLE_WGPU_CLASS

class GPUCompilationInfo final
  : public nsISupports
  , public nsWrapperCache
{
public:
  explicit GPUCompilationInfo(nsIGlobalObject* aGlobal);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetMessages(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUCompilationInfo)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUCompilationInfo() = default;
  nsCOMPtr<nsIGlobalObject> mOwner;
};

class GPUShaderModule final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUShaderModule(nsIGlobalObject* aGlobal, WGPUShaderModule aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUShaderModule Raw() const { return mRaw; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }
  already_AddRefed<Promise> GetCompilationInfo(ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUShaderModule)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUShaderModule();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUShaderModule mRaw;
  nsString mLabel;
};

class GPUComputePipeline final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUComputePipeline(nsIGlobalObject* aGlobal, WGPUComputePipeline aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUComputePipeline Raw() const { return mRaw; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }
  already_AddRefed<GPUBindGroupLayout> GetBindGroupLayout(uint32_t aIndex, ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUComputePipeline)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUComputePipeline();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUComputePipeline mRaw;
  nsString mLabel;
};

class GPURenderPipeline final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPURenderPipeline(nsIGlobalObject* aGlobal, WGPURenderPipeline aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPURenderPipeline Raw() const { return mRaw; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }
  already_AddRefed<GPUBindGroupLayout> GetBindGroupLayout(uint32_t aIndex, ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPURenderPipeline)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPURenderPipeline();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPURenderPipeline mRaw;
  nsString mLabel;
};

class GPUQuerySet final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUQuerySet(nsIGlobalObject* aGlobal, WGPUQuerySet aRaw, const nsAString& aType, uint32_t aCount);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUQuerySet Raw() const { return mRaw; }
  void GetType(nsAString& aOut) const { aOut = mType; }
  uint32_t Count() const { return mCount; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }
  void Destroy();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUQuerySet)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUQuerySet();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUQuerySet mRaw;
  nsString mType;
  uint32_t mCount;
  nsString mLabel;
};

class GPUCommandEncoder final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUCommandEncoder(nsIGlobalObject* aGlobal, WGPUCommandEncoder aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  WGPUCommandEncoder Raw() const { return mRaw; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  already_AddRefed<GPURenderPassEncoder> BeginRenderPass(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  already_AddRefed<GPUComputePassEncoder> BeginComputePass(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);
  void CopyBufferToBuffer(GPUBuffer& aSrc, uint64_t aSrcOff, GPUBuffer& aDst, uint64_t aDstOff, uint64_t aSize);
  void CopyBufferToTexture(JSContext* aCx, JS::Handle<JS::Value> aSrc, JS::Handle<JS::Value> aDst, JS::Handle<JS::Value> aSize, ErrorResult& aRv);
  void CopyTextureToBuffer(JSContext* aCx, JS::Handle<JS::Value> aSrc, JS::Handle<JS::Value> aDst, JS::Handle<JS::Value> aSize, ErrorResult& aRv);
  void CopyTextureToTexture(JSContext* aCx, JS::Handle<JS::Value> aSrc, JS::Handle<JS::Value> aDst, JS::Handle<JS::Value> aSize, ErrorResult& aRv);
  void ClearBuffer(GPUBuffer& aBuffer, uint64_t aOffset, const Optional<uint64_t>& aSize);
  void ResolveQuerySet(GPUQuerySet& aQuerySet, uint32_t aFirstQuery, uint32_t aQueryCount, GPUBuffer& aDst, uint64_t aDstOff);
  void PushDebugGroup(const nsAString& aLabel);
  void PopDebugGroup();
  void InsertDebugMarker(const nsAString& aLabel);
  already_AddRefed<GPUCommandBuffer> Finish(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUCommandEncoder)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUCommandEncoder();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUCommandEncoder mRaw;
  nsString mLabel;
};

class GPURenderPassEncoder final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPURenderPassEncoder(nsIGlobalObject* aGlobal, WGPURenderPassEncoder aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  void SetPipeline(GPURenderPipeline& aPipeline);
  void SetBindGroup(uint32_t aIndex, GPUBindGroup* aBindGroup, const Optional<Sequence<uint32_t>>& aDynamicOffsets);
  void SetVertexBuffer(uint32_t aSlot, GPUBuffer* aBuffer, uint64_t aOffset, const Optional<uint64_t>& aSize);
  void SetIndexBuffer(GPUBuffer& aBuffer, const nsAString& aIndexFormat, uint64_t aOffset, const Optional<uint64_t>& aSize);
  void Draw(uint32_t aVertexCount, uint32_t aInstanceCount, uint32_t aFirstVertex, uint32_t aFirstInstance);
  void DrawIndexed(uint32_t aIndexCount, uint32_t aInstanceCount, uint32_t aFirstIndex, int32_t aBaseVertex, uint32_t aFirstInstance);
  void DrawIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset);
  void DrawIndexedIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset);
  void SetViewport(float aX, float aY, float aW, float aH, float aMinD, float aMaxD);
  void SetScissorRect(uint32_t aX, uint32_t aY, uint32_t aW, uint32_t aH);
  void SetBlendConstant(JSContext* aCx, JS::Handle<JS::Value> aColor, ErrorResult& aRv);
  void SetStencilReference(uint32_t aRef);
  void ExecuteBundles(const Sequence<OwningNonNull<GPURenderBundle>>& aBundles);
  void BeginOcclusionQuery(uint32_t aQueryIndex);
  void EndOcclusionQuery();
  void PushDebugGroup(const nsAString& aLabel);
  void PopDebugGroup();
  void InsertDebugMarker(const nsAString& aLabel);
  void End();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPURenderPassEncoder)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPURenderPassEncoder();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPURenderPassEncoder mRaw;
  nsString mLabel;
};

class GPUComputePassEncoder final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPUComputePassEncoder(nsIGlobalObject* aGlobal, WGPUComputePassEncoder aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  void SetPipeline(GPUComputePipeline& aPipeline);
  void SetBindGroup(uint32_t aIndex, GPUBindGroup* aBindGroup, const Optional<Sequence<uint32_t>>& aDynamicOffsets);
  void DispatchWorkgroups(uint32_t aX, uint32_t aY, uint32_t aZ);
  void DispatchWorkgroupsIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset);
  void PushDebugGroup(const nsAString& aLabel);
  void PopDebugGroup();
  void InsertDebugMarker(const nsAString& aLabel);
  void End();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUComputePassEncoder)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUComputePassEncoder();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPUComputePassEncoder mRaw;
  nsString mLabel;
};

class GPURenderBundleEncoder final
  : public nsISupports
  , public nsWrapperCache
{
public:
  GPURenderBundleEncoder(nsIGlobalObject* aGlobal, WGPURenderBundleEncoder aRaw);
  nsIGlobalObject* GetParentObject() const { return mOwner; }
  void GetLabel(nsAString& aOut) const { aOut = mLabel; }
  void SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

  void SetPipeline(GPURenderPipeline& aPipeline);
  void SetBindGroup(uint32_t aIndex, GPUBindGroup* aBindGroup, const Optional<Sequence<uint32_t>>& aDynamicOffsets);
  void SetVertexBuffer(uint32_t aSlot, GPUBuffer* aBuffer, uint64_t aOffset, const Optional<uint64_t>& aSize);
  void SetIndexBuffer(GPUBuffer& aBuffer, const nsAString& aIndexFormat, uint64_t aOffset, const Optional<uint64_t>& aSize);
  void Draw(uint32_t aVertexCount, uint32_t aInstanceCount, uint32_t aFirstVertex, uint32_t aFirstInstance);
  void DrawIndexed(uint32_t aIndexCount, uint32_t aInstanceCount, uint32_t aFirstIndex, int32_t aBaseVertex, uint32_t aFirstInstance);
  void DrawIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset);
  void DrawIndexedIndirect(GPUBuffer& aIndirectBuffer, uint64_t aIndirectOffset);
  already_AddRefed<GPURenderBundle> Finish(JSContext* aCx, JS::Handle<JS::Value> aDesc, ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPURenderBundleEncoder)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPURenderBundleEncoder();
  nsCOMPtr<nsIGlobalObject> mOwner;
  WGPURenderBundleEncoder mRaw;
  nsString mLabel;
};

class GPUCanvasContext final
  : public nsICanvasRenderingContextInternal
  , public nsWrapperCache
{
public:
  GPUCanvasContext();

  HTMLCanvasElement* GetCanvas() const { return mCanvasElement; }

  void Configure(JSContext* aCx, JS::Handle<JS::Value> aConfiguration, ErrorResult& aRv);
  void Unconfigure();
  already_AddRefed<GPUTexture> GetCurrentTexture(ErrorResult& aRv);
  void GetConfiguration(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval);

  WGPUDevice ConfiguredDevice() const { return mDevice ? mDevice->Raw() : nullptr; }
  void PresentIfDirty();

  // nsICanvasRenderingContextInternal
  virtual int32_t GetWidth() const override { return mWidth; }
  virtual int32_t GetHeight() const override { return mHeight; }
  NS_IMETHOD SetDimensions(int32_t aWidth, int32_t aHeight) override;
  NS_IMETHOD InitializeWithDrawTarget(nsIDocShell* aDocShell, NotNull<gfx::DrawTarget*> aTarget) override;
  virtual mozilla::UniquePtr<uint8_t[]> GetImageBuffer(int32_t* aFormat) override;
  NS_IMETHOD GetInputStream(const char* aMimeType, const char16_t* aEncoderOptions, nsIInputStream** aStream) override;
  virtual already_AddRefed<mozilla::gfx::SourceSurface> GetSurfaceSnapshot(bool* aPremultAlpha = nullptr) override;
  NS_IMETHOD SetIsOpaque(bool aIsOpaque) override { mIsOpaque = aIsOpaque; return NS_OK; }
  virtual bool GetIsOpaque() override { return mIsOpaque; }
  NS_IMETHOD Reset() override;
  virtual already_AddRefed<layers::Layer> GetCanvasLayer(nsDisplayListBuilder* aBuilder,
                                                         layers::Layer* aOldLayer,
                                                         layers::LayerManager* aManager,
                                                         bool aMirror = false) override;
  virtual void MarkContextClean() override {}
  virtual void MarkContextCleanForFrameCapture() override {}
  virtual bool IsContextCleanForFrameCapture() override { return true; }
  NS_IMETHOD Redraw(const gfxRect& aDirty) override;
  NS_IMETHOD SetIsIPC(bool aIsIPC) override { return NS_OK; }
  virtual void DidRefresh() override {}
  NS_IMETHOD SetContextOptions(JSContext* aCx, JS::Handle<JS::Value> aOptions, ErrorResult& aRvForDictionaryInit) override { return NS_OK; }

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(GPUCanvasContext)
  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~GPUCanvasContext();
  void ReleaseSwapchainResources();
  bool EnsureSwapchainTexture();
  bool DrainCompletedReadbacks(bool aWait);

  static const uint32_t kNumReadbackSlots = 3;
  struct ReadbackSlot {
    WGPUBuffer buffer = nullptr;
    bool inFlight = false;
    bool mappedOk = false;
    uint64_t frameSeq = 0;
  };

  int32_t mWidth;
  int32_t mHeight;
  bool mIsOpaque;
  bool mConfigured;
  bool mTextureAcquiredThisFrame;
  RefPtr<GPUDevice> mDevice;
  nsString mFormatStr;
  WGPUTextureFormat mFormat;
  uint32_t mUsage;
  WGPUTexture mSwapTexture;
  ReadbackSlot mReadbackSlots[kNumReadbackSlots];
  uint64_t mNextFrameSeq;
  uint32_t mTexWidth;
  uint32_t mTexHeight;
  uint32_t mBytesPerRow;
  RefPtr<gfx::DataSourceSurface> mSurface;
  RefPtr<layers::ImageContainer> mImageContainer;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_WebGPU_h
