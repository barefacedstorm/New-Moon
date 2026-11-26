/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

[Exposed=(Window,Worker), SecureContext]
interface mixin NavigatorGPU {
  [SameObject]
  readonly attribute GPU gpu;
};

[Exposed=(Window,Worker)]
namespace GPUBufferUsage {
  const unsigned long MAP_READ      = 0x0001;
  const unsigned long MAP_WRITE     = 0x0002;
  const unsigned long COPY_SRC      = 0x0004;
  const unsigned long COPY_DST      = 0x0008;
  const unsigned long INDEX         = 0x0010;
  const unsigned long VERTEX        = 0x0020;
  const unsigned long UNIFORM       = 0x0040;
  const unsigned long STORAGE       = 0x0080;
  const unsigned long INDIRECT      = 0x0100;
  const unsigned long QUERY_RESOLVE = 0x0200;
};

[Exposed=(Window,Worker)]
namespace GPUMapMode {
  const unsigned long READ  = 0x0001;
  const unsigned long WRITE = 0x0002;
};

[Exposed=(Window,Worker)]
namespace GPUTextureUsage {
  const unsigned long COPY_SRC          = 0x01;
  const unsigned long COPY_DST          = 0x02;
  const unsigned long TEXTURE_BINDING   = 0x04;
  const unsigned long STORAGE_BINDING   = 0x08;
  const unsigned long RENDER_ATTACHMENT = 0x10;
};

[Exposed=(Window,Worker)]
namespace GPUShaderStage {
  const unsigned long VERTEX   = 0x1;
  const unsigned long FRAGMENT = 0x2;
  const unsigned long COMPUTE  = 0x4;
};

[Exposed=(Window,Worker)]
namespace GPUColorWrite {
  const unsigned long RED   = 0x1;
  const unsigned long GREEN = 0x2;
  const unsigned long BLUE  = 0x4;
  const unsigned long ALPHA = 0x8;
  const unsigned long ALL   = 0xF;
};

[Exposed=(Window,Worker), SecureContext]
interface GPU {
  [Throws]
  Promise<GPUAdapter?> requestAdapter(optional any options);
  DOMString getPreferredCanvasFormat();
  readonly attribute any wgslLanguageFeatures;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUAdapterInfo {
  readonly attribute DOMString vendor;
  readonly attribute DOMString architecture;
  readonly attribute DOMString device;
  readonly attribute DOMString description;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUSupportedLimits {
  readonly attribute unsigned long maxTextureDimension1D;
  readonly attribute unsigned long maxTextureDimension2D;
  readonly attribute unsigned long maxTextureDimension3D;
  readonly attribute unsigned long maxTextureArrayLayers;
  readonly attribute unsigned long maxBindGroups;
  readonly attribute unsigned long maxBindGroupsPlusVertexBuffers;
  readonly attribute unsigned long maxBindingsPerBindGroup;
  readonly attribute unsigned long maxDynamicUniformBuffersPerPipelineLayout;
  readonly attribute unsigned long maxDynamicStorageBuffersPerPipelineLayout;
  readonly attribute unsigned long maxSampledTexturesPerShaderStage;
  readonly attribute unsigned long maxSamplersPerShaderStage;
  readonly attribute unsigned long maxStorageBuffersPerShaderStage;
  readonly attribute unsigned long maxStorageTexturesPerShaderStage;
  readonly attribute unsigned long maxUniformBuffersPerShaderStage;
  readonly attribute unsigned long long maxUniformBufferBindingSize;
  readonly attribute unsigned long long maxStorageBufferBindingSize;
  readonly attribute unsigned long minUniformBufferOffsetAlignment;
  readonly attribute unsigned long minStorageBufferOffsetAlignment;
  readonly attribute unsigned long maxVertexBuffers;
  readonly attribute unsigned long long maxBufferSize;
  readonly attribute unsigned long maxVertexAttributes;
  readonly attribute unsigned long maxVertexBufferArrayStride;
  readonly attribute unsigned long maxInterStageShaderComponents;
  readonly attribute unsigned long maxInterStageShaderVariables;
  readonly attribute unsigned long maxColorAttachments;
  readonly attribute unsigned long maxColorAttachmentBytesPerSample;
  readonly attribute unsigned long maxComputeWorkgroupStorageSize;
  readonly attribute unsigned long maxComputeInvocationsPerWorkgroup;
  readonly attribute unsigned long maxComputeWorkgroupSizeX;
  readonly attribute unsigned long maxComputeWorkgroupSizeY;
  readonly attribute unsigned long maxComputeWorkgroupSizeZ;
  readonly attribute unsigned long maxComputeWorkgroupsPerDimension;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUAdapter {
  readonly attribute any features;
  readonly attribute GPUSupportedLimits limits;
  readonly attribute GPUAdapterInfo info;
  readonly attribute boolean isFallbackAdapter;

  [Throws]
  Promise<GPUDevice> requestDevice(optional any descriptor);
  [Throws]
  Promise<GPUAdapterInfo> requestAdapterInfo();
};

[Exposed=(Window,Worker), SecureContext]
interface GPUDeviceLostInfo {
  readonly attribute DOMString reason;
  readonly attribute DOMString message;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUError {
  readonly attribute DOMString message;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUDevice : EventTarget {
  readonly attribute any features;
  readonly attribute GPUSupportedLimits limits;
  readonly attribute GPUQueue queue;
  [Throws]
  readonly attribute Promise<GPUDeviceLostInfo> lost;
  attribute USVString label;
  attribute EventHandler onuncapturederror;

  void destroy();

  [Throws]
  GPUBuffer createBuffer(any descriptor);
  [Throws]
  GPUTexture createTexture(any descriptor);
  [Throws]
  GPUSampler createSampler(optional any descriptor);
  [Throws]
  GPUBindGroupLayout createBindGroupLayout(any descriptor);
  [Throws]
  GPUPipelineLayout createPipelineLayout(any descriptor);
  [Throws]
  GPUBindGroup createBindGroup(any descriptor);
  [Throws]
  GPUShaderModule createShaderModule(any descriptor);
  [Throws]
  GPUComputePipeline createComputePipeline(any descriptor);
  [Throws]
  Promise<GPUComputePipeline> createComputePipelineAsync(any descriptor);
  [Throws]
  GPURenderPipeline createRenderPipeline(any descriptor);
  [Throws]
  Promise<GPURenderPipeline> createRenderPipelineAsync(any descriptor);
  [Throws]
  GPUCommandEncoder createCommandEncoder(optional any descriptor);
  [Throws]
  GPURenderBundleEncoder createRenderBundleEncoder(any descriptor);
  [Throws]
  GPUQuerySet createQuerySet(any descriptor);

  void pushErrorScope(DOMString filter);
  [Throws]
  Promise<GPUError?> popErrorScope();
};

[Exposed=(Window,Worker), SecureContext]
interface GPUQueue {
  attribute USVString label;

  void submit(sequence<GPUCommandBuffer> commandBuffers);
  [Throws]
  Promise<void> onSubmittedWorkDone();
  [Throws]
  void writeBuffer(GPUBuffer buffer,
                   unsigned long long bufferOffset,
                   any data,
                   optional unsigned long long dataOffset = 0,
                   optional unsigned long long size);
  [Throws]
  void writeTexture(any destination,
                    any data,
                    any dataLayout,
                    any size);
  [Throws]
  void copyExternalImageToTexture(any source, any destination, any copySize);
};

[Exposed=(Window,Worker), SecureContext]
interface GPUBuffer {
  readonly attribute unsigned long long size;
  readonly attribute unsigned long usage;
  readonly attribute DOMString mapState;
  attribute USVString label;

  [Throws]
  Promise<void> mapAsync(unsigned long mode,
                         optional unsigned long long offset = 0,
                         optional unsigned long long size);
  [Throws]
  any getMappedRange(optional unsigned long long offset = 0,
                     optional unsigned long long size);
  void unmap();
  void destroy();
};

[Exposed=(Window,Worker), SecureContext]
interface GPUTexture {
  readonly attribute unsigned long width;
  readonly attribute unsigned long height;
  readonly attribute unsigned long depthOrArrayLayers;
  readonly attribute unsigned long mipLevelCount;
  readonly attribute unsigned long sampleCount;
  readonly attribute DOMString dimension;
  readonly attribute DOMString format;
  readonly attribute unsigned long usage;
  attribute USVString label;

  [Throws]
  GPUTextureView createView(optional any descriptor);
  void destroy();
};

[Exposed=(Window,Worker), SecureContext]
interface GPUTextureView {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUSampler {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUBindGroupLayout {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUPipelineLayout {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUBindGroup {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUCompilationInfo {
  readonly attribute any messages;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUShaderModule {
  attribute USVString label;
  [Throws]
  Promise<GPUCompilationInfo> getCompilationInfo();
};

[Exposed=(Window,Worker), SecureContext]
interface GPUComputePipeline {
  attribute USVString label;
  [Throws]
  GPUBindGroupLayout getBindGroupLayout(unsigned long index);
};

[Exposed=(Window,Worker), SecureContext]
interface GPURenderPipeline {
  attribute USVString label;
  [Throws]
  GPUBindGroupLayout getBindGroupLayout(unsigned long index);
};

[Exposed=(Window,Worker), SecureContext]
interface GPUCommandBuffer {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPUCommandEncoder {
  attribute USVString label;

  [Throws]
  GPURenderPassEncoder beginRenderPass(any descriptor);
  [Throws]
  GPUComputePassEncoder beginComputePass(optional any descriptor);

  void copyBufferToBuffer(GPUBuffer source,
                          unsigned long long sourceOffset,
                          GPUBuffer destination,
                          unsigned long long destinationOffset,
                          unsigned long long size);
  [Throws]
  void copyBufferToTexture(any source, any destination, any copySize);
  [Throws]
  void copyTextureToBuffer(any source, any destination, any copySize);
  [Throws]
  void copyTextureToTexture(any source, any destination, any copySize);
  void clearBuffer(GPUBuffer buffer,
                   optional unsigned long long offset = 0,
                   optional unsigned long long size);
  void resolveQuerySet(GPUQuerySet querySet,
                       unsigned long firstQuery,
                       unsigned long queryCount,
                       GPUBuffer destination,
                       unsigned long long destinationOffset);
  void pushDebugGroup(USVString groupLabel);
  void popDebugGroup();
  void insertDebugMarker(USVString markerLabel);
  [Throws]
  GPUCommandBuffer finish(optional any descriptor);
};

[Exposed=(Window,Worker), SecureContext]
interface GPURenderPassEncoder {
  attribute USVString label;

  void setPipeline(GPURenderPipeline pipeline);
  void setBindGroup(unsigned long index,
                    GPUBindGroup? bindGroup,
                    optional sequence<unsigned long> dynamicOffsets);
  void setVertexBuffer(unsigned long slot,
                       GPUBuffer? buffer,
                       optional unsigned long long offset = 0,
                       optional unsigned long long size);
  void setIndexBuffer(GPUBuffer buffer,
                      DOMString indexFormat,
                      optional unsigned long long offset = 0,
                      optional unsigned long long size);
  void draw(unsigned long vertexCount,
            optional unsigned long instanceCount = 1,
            optional unsigned long firstVertex = 0,
            optional unsigned long firstInstance = 0);
  void drawIndexed(unsigned long indexCount,
                   optional unsigned long instanceCount = 1,
                   optional unsigned long firstIndex = 0,
                   optional long baseVertex = 0,
                   optional unsigned long firstInstance = 0);
  void drawIndirect(GPUBuffer indirectBuffer, unsigned long long indirectOffset);
  void drawIndexedIndirect(GPUBuffer indirectBuffer, unsigned long long indirectOffset);
  void setViewport(float x, float y, float width, float height, float minDepth, float maxDepth);
  void setScissorRect(unsigned long x, unsigned long y, unsigned long width, unsigned long height);
  [Throws]
  void setBlendConstant(any color);
  void setStencilReference(unsigned long reference);
  void executeBundles(sequence<GPURenderBundle> bundles);
  void beginOcclusionQuery(unsigned long queryIndex);
  void endOcclusionQuery();
  void pushDebugGroup(USVString groupLabel);
  void popDebugGroup();
  void insertDebugMarker(USVString markerLabel);
  void end();
};

[Exposed=(Window,Worker), SecureContext]
interface GPUComputePassEncoder {
  attribute USVString label;

  void setPipeline(GPUComputePipeline pipeline);
  void setBindGroup(unsigned long index,
                    GPUBindGroup? bindGroup,
                    optional sequence<unsigned long> dynamicOffsets);
  void dispatchWorkgroups(unsigned long workgroupCountX,
                          optional unsigned long workgroupCountY = 1,
                          optional unsigned long workgroupCountZ = 1);
  void dispatchWorkgroupsIndirect(GPUBuffer indirectBuffer,
                                  unsigned long long indirectOffset);
  void pushDebugGroup(USVString groupLabel);
  void popDebugGroup();
  void insertDebugMarker(USVString markerLabel);
  void end();
};

[Exposed=(Window,Worker), SecureContext]
interface GPURenderBundle {
  attribute USVString label;
};

[Exposed=(Window,Worker), SecureContext]
interface GPURenderBundleEncoder {
  attribute USVString label;

  void setPipeline(GPURenderPipeline pipeline);
  void setBindGroup(unsigned long index,
                    GPUBindGroup? bindGroup,
                    optional sequence<unsigned long> dynamicOffsets);
  void setVertexBuffer(unsigned long slot,
                       GPUBuffer? buffer,
                       optional unsigned long long offset = 0,
                       optional unsigned long long size);
  void setIndexBuffer(GPUBuffer buffer,
                      DOMString indexFormat,
                      optional unsigned long long offset = 0,
                      optional unsigned long long size);
  void draw(unsigned long vertexCount,
            optional unsigned long instanceCount = 1,
            optional unsigned long firstVertex = 0,
            optional unsigned long firstInstance = 0);
  void drawIndexed(unsigned long indexCount,
                   optional unsigned long instanceCount = 1,
                   optional unsigned long firstIndex = 0,
                   optional long baseVertex = 0,
                   optional unsigned long firstInstance = 0);
  void drawIndirect(GPUBuffer indirectBuffer, unsigned long long indirectOffset);
  void drawIndexedIndirect(GPUBuffer indirectBuffer, unsigned long long indirectOffset);
  [Throws]
  GPURenderBundle finish(optional any descriptor);
};

[Exposed=(Window,Worker), SecureContext]
interface GPUQuerySet {
  readonly attribute DOMString type;
  readonly attribute unsigned long count;
  attribute USVString label;
  void destroy();
};

[Exposed=Window, SecureContext]
interface GPUCanvasContext {
  readonly attribute HTMLCanvasElement? canvas;

  [Throws]
  void configure(any configuration);
  void unconfigure();
  [Throws]
  GPUTexture getCurrentTexture();
  any getConfiguration();
};
