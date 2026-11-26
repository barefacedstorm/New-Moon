/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebGPU.h"
#include "WebGPUHelpers.h"

#include "ImageContainer.h"
#include "ImageEncoder.h"
#include "ImageLayers.h"
#include "Layers.h"
#include "imgIEncoder.h"
#include "mozilla/Mutex.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/dom/WebGPUBinding.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/DataSurfaceHelpers.h"

#include <string.h>

using namespace mozilla::gfx;
using namespace mozilla::layers;
using namespace mozilla::dom::webgpu_helpers;

namespace mozilla {
namespace dom {

namespace {

static StaticMutex sCanvasRegistryMutex;
static nsTArray<GPUCanvasContext*>* sActiveCanvasContexts = nullptr;

static void
OnReadbackMap(WGPUMapAsyncStatus aStatus,
              WGPUStringView aMessage,
              void* aUserdata1,
              void* aUserdata2)
{
  bool* flags = static_cast<bool*>(aUserdata1);
  // userdata1 = &slot.inFlight, userdata2 = &slot.mappedOk
  if (flags) {
    *flags = false;
  }
  bool* ok = static_cast<bool*>(aUserdata2);
  if (ok) {
    *ok = (aStatus == WGPUMapAsyncStatus_Success);
  }
}

} // anonymous namespace

void
RegisterWebGPUCanvasContext(GPUCanvasContext* aCtx)
{
  StaticMutexAutoLock lock(sCanvasRegistryMutex);
  if (!sActiveCanvasContexts) {
    sActiveCanvasContexts = new nsTArray<GPUCanvasContext*>();
  }
  if (!sActiveCanvasContexts->Contains(aCtx)) {
    sActiveCanvasContexts->AppendElement(aCtx);
  }
}

void
UnregisterWebGPUCanvasContext(GPUCanvasContext* aCtx)
{
  StaticMutexAutoLock lock(sCanvasRegistryMutex);
  if (sActiveCanvasContexts) {
    sActiveCanvasContexts->RemoveElement(aCtx);
  }
}

void
PresentAllWebGPUCanvasContextsForDevice(WGPUDevice aDevice)
{
  nsTArray<RefPtr<GPUCanvasContext>> snapshot;
  {
    StaticMutexAutoLock lock(sCanvasRegistryMutex);
    if (sActiveCanvasContexts) {
      for (GPUCanvasContext* ctx : *sActiveCanvasContexts) {
        if (ctx && ctx->ConfiguredDevice() == aDevice) {
          snapshot.AppendElement(ctx);
        }
      }
    }
  }
  for (auto& ctx : snapshot) {
    ctx->PresentIfDirty();
  }
}

GPUCanvasContext::GPUCanvasContext()
  : mWidth(300)
  , mHeight(150)
  , mIsOpaque(false)
  , mConfigured(false)
  , mTextureAcquiredThisFrame(false)
  , mFormat(WGPUTextureFormat_BGRA8Unorm)
  , mUsage(0x10)
  , mSwapTexture(nullptr)
  , mNextFrameSeq(1)
  , mTexWidth(0)
  , mTexHeight(0)
  , mBytesPerRow(0)
{
  mFormatStr.AssignLiteral("bgra8unorm");
  RegisterWebGPUCanvasContext(this);
}

GPUCanvasContext::~GPUCanvasContext()
{
  UnregisterWebGPUCanvasContext(this);
  ReleaseSwapchainResources();
  RemovePostRefreshObserver();
}

void
GPUCanvasContext::ReleaseSwapchainResources()
{
  // Flush any in-flight async map callbacks before destroying buffers.
  bool anyInFlight = false;
  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    if (mReadbackSlots[i].inFlight) {
      anyInFlight = true;
      break;
    }
  }
  if (anyInFlight && mDevice && mDevice->Raw()) {
    wgpuDevicePoll(mDevice->Raw(), true, nullptr);
  }
  if (mSwapTexture) {
    wgpuTextureDestroy(mSwapTexture);
    wgpuTextureRelease(mSwapTexture);
    mSwapTexture = nullptr;
  }
  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    if (mReadbackSlots[i].buffer) {
      if (mReadbackSlots[i].mappedOk) {
        wgpuBufferUnmap(mReadbackSlots[i].buffer);
      }
      wgpuBufferDestroy(mReadbackSlots[i].buffer);
      wgpuBufferRelease(mReadbackSlots[i].buffer);
      mReadbackSlots[i].buffer = nullptr;
    }
    mReadbackSlots[i].inFlight = false;
    mReadbackSlots[i].mappedOk = false;
    mReadbackSlots[i].frameSeq = 0;
  }
  mTexWidth = 0;
  mTexHeight = 0;
  mBytesPerRow = 0;
  mTextureAcquiredThisFrame = false;
}

bool
GPUCanvasContext::EnsureSwapchainTexture()
{
  if (!mConfigured || !mDevice || !mDevice->Raw()) {
    return false;
  }
  uint32_t w = (mWidth > 0) ? static_cast<uint32_t>(mWidth) : 1;
  uint32_t h = (mHeight > 0) ? static_cast<uint32_t>(mHeight) : 1;

  if (mSwapTexture && mTexWidth == w && mTexHeight == h) {
    return true;
  }

  ReleaseSwapchainResources();

  WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
  td.usage = static_cast<WGPUTextureUsage>(mUsage) |
             WGPUTextureUsage_CopySrc |
             WGPUTextureUsage_RenderAttachment;
  td.dimension = WGPUTextureDimension_2D;
  td.size = { w, h, 1 };
  td.format = mFormat;
  td.mipLevelCount = 1;
  td.sampleCount = 1;

  mSwapTexture = wgpuDeviceCreateTexture(mDevice->Raw(), &td);
  if (!mSwapTexture) {
    return false;
  }

  // WebGPU requires bytesPerRow to be a multiple of 256.
  uint32_t unaligned = w * 4;
  mBytesPerRow = (unaligned + 255) & ~255u;

  WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  bd.size = static_cast<uint64_t>(mBytesPerRow) * static_cast<uint64_t>(h);
  bd.mappedAtCreation = false;

  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    mReadbackSlots[i].buffer = wgpuDeviceCreateBuffer(mDevice->Raw(), &bd);
    mReadbackSlots[i].inFlight = false;
    mReadbackSlots[i].mappedOk = false;
    mReadbackSlots[i].frameSeq = 0;
    if (!mReadbackSlots[i].buffer) {
      ReleaseSwapchainResources();
      return false;
    }
  }

  mTexWidth = w;
  mTexHeight = h;
  return true;
}

void
GPUCanvasContext::Configure(JSContext* aCx,
                            JS::Handle<JS::Value> aConfiguration,
                            ErrorResult& aRv)
{
  if (!aConfiguration.isObject()) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }
  JS::Rooted<JSObject*> obj(aCx, &aConfiguration.toObject());
  JS::Rooted<JSObject*> devObj(aCx);
  if (!GetPropObj(aCx, obj, "device", &devObj)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }
  RefPtr<GPUDevice> dev;
  if (NS_FAILED(UNWRAP_OBJECT(GPUDevice, devObj, dev)) || !dev) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return;
  }

  nsCString fmtStr;
  if (!GetPropString(aCx, obj, "format", fmtStr) || fmtStr.IsEmpty()) {
    fmtStr.AssignLiteral("bgra8unorm");
  }
  WGPUTextureFormat fmt = ParseTextureFormat(fmtStr);
  if (fmt == WGPUTextureFormat_Undefined) {
    fmt = WGPUTextureFormat_BGRA8Unorm;
    fmtStr.AssignLiteral("bgra8unorm");
  }

  uint32_t usage = 0x10;
  GetPropU32(aCx, obj, "usage", usage);

  ReleaseSwapchainResources();
  mDevice = dev;
  mFormat = fmt;
  mFormatStr = NS_ConvertUTF8toUTF16(fmtStr);
  mUsage = usage;
  mConfigured = true;

  EnsureSwapchainTexture();
}

void
GPUCanvasContext::Unconfigure()
{
  ReleaseSwapchainResources();
  mDevice = nullptr;
  mConfigured = false;
  mSurface = nullptr;
}

already_AddRefed<GPUTexture>
GPUCanvasContext::GetCurrentTexture(ErrorResult& aRv)
{
  if (!mConfigured || !EnsureSwapchainTexture()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }
  mTextureAcquiredThisFrame = true;
  wgpuTextureAddRef(mSwapTexture);
  nsCOMPtr<nsIGlobalObject> global = mDevice ? mDevice->GetParentObject() : nullptr;
  RefPtr<GPUTexture> tex = new GPUTexture(
    global, mSwapTexture, mTexWidth, mTexHeight, 1, 1, 1,
    NS_LITERAL_STRING("2d"), mFormatStr, mUsage, /* aOwned = */ true);
  return tex.forget();
}

void
GPUCanvasContext::GetConfiguration(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval)
{
  if (!mConfigured) {
    aRetval.setNull();
    return;
  }
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
  if (!obj) {
    aRetval.setNull();
    return;
  }
  JS::Rooted<JS::Value> fmtVal(aCx);
  if (ToJSValue(aCx, mFormatStr, &fmtVal)) {
    JS_DefineProperty(aCx, obj, "format", fmtVal, JSPROP_ENUMERATE);
  }
  JS::Rooted<JS::Value> usageVal(aCx, JS::NumberValue(mUsage));
  JS_DefineProperty(aCx, obj, "usage", usageVal, JSPROP_ENUMERATE);
  aRetval.setObject(*obj);
}

bool
GPUCanvasContext::DrainCompletedReadbacks(bool aWait)
{
  if (!mDevice || !mDevice->Raw()) {
    return false;
  }
  wgpuDevicePoll(mDevice->Raw(), aWait, nullptr);

  int bestIdx = -1;
  uint64_t bestSeq = 0;
  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    if (!mReadbackSlots[i].inFlight && mReadbackSlots[i].mappedOk && mReadbackSlots[i].buffer) {
      if (bestIdx < 0 || mReadbackSlots[i].frameSeq > bestSeq) {
        bestIdx = static_cast<int>(i);
        bestSeq = mReadbackSlots[i].frameSeq;
      }
    }
  }
  if (bestIdx < 0) {
    return false;
  }

  uint64_t mapSize = static_cast<uint64_t>(mBytesPerRow) * static_cast<uint64_t>(mTexHeight);
  const uint8_t* mapped = static_cast<const uint8_t*>(
    wgpuBufferGetConstMappedRange(mReadbackSlots[bestIdx].buffer, 0, mapSize));
  if (mapped) {
    SurfaceFormat surfFmt = mIsOpaque ? SurfaceFormat::B8G8R8X8 : SurfaceFormat::B8G8R8A8;
    if (!mSurface || mSurface->GetSize() != IntSize(mTexWidth, mTexHeight) ||
        mSurface->GetFormat() != surfFmt) {
      mSurface = Factory::CreateDataSourceSurface(IntSize(mTexWidth, mTexHeight), surfFmt);
    }
    if (mSurface) {
      DataSourceSurface::ScopedMap map(mSurface, DataSourceSurface::WRITE);
      if (map.IsMapped()) {
        bool isRGBA = (mFormat == WGPUTextureFormat_RGBA8Unorm ||
                       mFormat == WGPUTextureFormat_RGBA8UnormSrgb);
        int32_t dstStride = map.GetStride();
        if (!isRGBA && static_cast<uint32_t>(dstStride) == mBytesPerRow) {
          memcpy(map.GetData(), mapped, static_cast<size_t>(mapSize));
        } else {
          for (uint32_t y = 0; y < mTexHeight; ++y) {
            const uint8_t* srcRow = mapped + static_cast<size_t>(y) * mBytesPerRow;
            uint8_t* dstRow = map.GetData() + static_cast<size_t>(y) * dstStride;
            if (!isRGBA) {
              memcpy(dstRow, srcRow, mTexWidth * 4);
            } else {
              for (uint32_t x = 0; x < mTexWidth; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
              }
            }
          }
        }
      }
    }
  }

  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    if (!mReadbackSlots[i].inFlight && mReadbackSlots[i].mappedOk && mReadbackSlots[i].buffer) {
      wgpuBufferUnmap(mReadbackSlots[i].buffer);
      mReadbackSlots[i].mappedOk = false;
    }
  }

  Redraw(gfxRect(0, 0, mWidth, mHeight));
  return true;
}

void
GPUCanvasContext::PresentIfDirty()
{
  if (!mTextureAcquiredThisFrame || !mConfigured || !mDevice || !mDevice->Raw() ||
      !mSwapTexture || !mReadbackSlots[0].buffer) {
    return;
  }
  mTextureAcquiredThisFrame = false;

  WGPUDevice rawDev = mDevice->Raw();
  WGPUQueue rawQueue = mDevice->Queue() ? mDevice->Queue()->Raw() : nullptr;
  if (!rawQueue) {
    return;
  }

  // First, non-blockingly drain any completed readbacks from prior frames.
  DrainCompletedReadbacks(false);

  // Pick a free readback slot; if all 3 are in flight, wait once to free a slot.
  int freeIdx = -1;
  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    if (!mReadbackSlots[i].inFlight && !mReadbackSlots[i].mappedOk) {
      freeIdx = static_cast<int>(i);
      break;
    }
  }
  if (freeIdx < 0) {
    DrainCompletedReadbacks(true);
    for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
      if (!mReadbackSlots[i].inFlight && !mReadbackSlots[i].mappedOk) {
        freeIdx = static_cast<int>(i);
        break;
      }
    }
  }
  if (freeIdx < 0) {
    return;
  }

  ReadbackSlot& slot = mReadbackSlots[freeIdx];
  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(rawDev, &ced);
  if (!enc) {
    return;
  }

  WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  src.texture = mSwapTexture;
  src.mipLevel = 0;
  src.origin = { 0, 0, 0 };
  src.aspect = WGPUTextureAspect_All;

  WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
  dst.buffer = slot.buffer;
  dst.layout.offset = 0;
  dst.layout.bytesPerRow = mBytesPerRow;
  dst.layout.rowsPerImage = mTexHeight;

  WGPUExtent3D extent = { mTexWidth, mTexHeight, 1 };
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);

  WGPUCommandBufferDescriptor cbd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cbd);
  wgpuCommandEncoderRelease(enc);
  if (!cmd) {
    return;
  }

  wgpuQueueSubmit(rawQueue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);

  uint64_t mapSize = static_cast<uint64_t>(mBytesPerRow) * static_cast<uint64_t>(mTexHeight);
  slot.inFlight = true;
  slot.mappedOk = false;
  slot.frameSeq = mNextFrameSeq++;

  WGPUBufferMapCallbackInfo cbInfo = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  cbInfo.mode = WGPUCallbackMode_AllowSpontaneous;
  cbInfo.callback = OnReadbackMap;
  cbInfo.userdata1 = &slot.inFlight;
  cbInfo.userdata2 = &slot.mappedOk;

  wgpuBufferMapAsync(slot.buffer, WGPUMapMode_Read, 0, mapSize, cbInfo);
  wgpuDevicePoll(rawDev, false, nullptr);
}

NS_IMETHODIMP
GPUCanvasContext::SetDimensions(int32_t aWidth, int32_t aHeight)
{
  mWidth = aWidth;
  mHeight = aHeight;
  if (mConfigured) {
    EnsureSwapchainTexture();
  }
  return NS_OK;
}

NS_IMETHODIMP
GPUCanvasContext::InitializeWithDrawTarget(nsIDocShell* aDocShell,
                                           NotNull<gfx::DrawTarget*> aTarget)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

mozilla::UniquePtr<uint8_t[]>
GPUCanvasContext::GetImageBuffer(int32_t* aFormat)
{
  *aFormat = 0;
  if (!mSurface) {
    return nullptr;
  }
  *aFormat = imgIEncoder::INPUT_FORMAT_HOSTARGB;
  return SurfaceToPackedBGRA(mSurface);
}

NS_IMETHODIMP
GPUCanvasContext::GetInputStream(const char* aMimeType,
                                 const char16_t* aEncoderOptions,
                                 nsIInputStream** aStream)
{
  nsCString enccid("@mozilla.org/image/encoder;2?type=");
  enccid += aMimeType;
  nsCOMPtr<imgIEncoder> encoder = do_CreateInstance(enccid.get());
  if (!encoder) {
    return NS_ERROR_FAILURE;
  }
  int32_t format = 0;
  UniquePtr<uint8_t[]> imageBuffer = GetImageBuffer(&format);
  if (!imageBuffer) {
    return NS_ERROR_FAILURE;
  }
  return ImageEncoder::GetInputStream(mWidth, mHeight, imageBuffer.get(), format,
                                      encoder, aEncoderOptions, aStream);
}

already_AddRefed<mozilla::gfx::SourceSurface>
GPUCanvasContext::GetSurfaceSnapshot(bool* aPremultAlpha)
{
  // Flush any pending readback so drawImage(canvas) / toDataURL sees the latest frame.
  for (uint32_t i = 0; i < kNumReadbackSlots; ++i) {
    if (mReadbackSlots[i].inFlight || mReadbackSlots[i].mappedOk) {
      DrainCompletedReadbacks(true);
      break;
    }
  }
  if (aPremultAlpha) {
    *aPremultAlpha = true;
  }
  RefPtr<SourceSurface> surf = mSurface;
  return surf.forget();
}

NS_IMETHODIMP
GPUCanvasContext::Reset()
{
  if (mCanvasElement) {
    mCanvasElement->InvalidateCanvas();
  }
  mSurface = nullptr;
  return NS_OK;
}

already_AddRefed<Layer>
GPUCanvasContext::GetCanvasLayer(nsDisplayListBuilder* aBuilder,
                                 Layer* aOldLayer,
                                 LayerManager* aManager,
                                 bool aMirror)
{
  if (aMirror || !mSurface) {
    return nullptr;
  }

  RefPtr<ImageLayer> imageLayer;
  if (aOldLayer && aOldLayer->GetType() == Layer::TYPE_IMAGE) {
    imageLayer = static_cast<ImageLayer*>(aOldLayer);
  } else {
    imageLayer = aManager->CreateImageLayer();
  }
  if (!imageLayer) {
    return nullptr;
  }

  if (!mImageContainer) {
    mImageContainer = LayerManager::CreateImageContainer();
  }

  RefPtr<Image> img = new SourceSurfaceImage(mSurface->GetSize(), mSurface);
  mImageContainer->SetCurrentImageInTransaction(img);
  imageLayer->SetContainer(mImageContainer);

  return imageLayer.forget();
}

NS_IMETHODIMP
GPUCanvasContext::Redraw(const gfxRect& aDirty)
{
  if (mCanvasElement) {
    gfx::Rect rect(aDirty.x, aDirty.y, aDirty.width, aDirty.height);
    mCanvasElement->InvalidateCanvasContent(&rect);
  }
  return NS_OK;
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(GPUCanvasContext)
NS_IMPL_CYCLE_COLLECTING_RELEASE(GPUCanvasContext)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(GPUCanvasContext,
                                      mCanvasElement,
                                      mDevice)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GPUCanvasContext)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsICanvasRenderingContextInternal)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
GPUCanvasContext::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return GPUCanvasContextBinding::Wrap(aCx, this, aGivenProto);
}

} // namespace dom
} // namespace mozilla
