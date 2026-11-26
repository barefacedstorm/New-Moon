/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WebGPUHelpers_h
#define mozilla_dom_WebGPUHelpers_h

#include "jsapi.h"
#include "nsJSUtils.h"
#include "nsString.h"
#pragma GCC visibility push(default)
#include "webgpu/webgpu.h"
#pragma GCC visibility pop
#include <string.h>

namespace mozilla {
namespace dom {
namespace webgpu_helpers {

inline WGPUStringView ToWGPUString(const nsCString& aStr) {
  WGPUStringView sv;
  sv.data = aStr.IsEmpty() ? nullptr : aStr.get();
  sv.length = aStr.Length();
  return sv;
}

inline bool GetPropVal(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, JS::MutableHandle<JS::Value> aOut) {
  if (!aObj || !JS_GetProperty(aCx, aObj, aName, aOut)) {
    JS_ClearPendingException(aCx);
    return false;
  }
  return !aOut.isUndefined() && !aOut.isNull();
}

inline bool GetPropObj(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, JS::MutableHandle<JSObject*> aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v) || !v.isObject()) {
    return false;
  }
  aOut.set(&v.toObject());
  return true;
}

inline bool GetPropString(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, nsCString& aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v)) {
    return false;
  }
  JS::Rooted<JSString*> jsStr(aCx, JS::ToString(aCx, v));
  if (!jsStr) {
    JS_ClearPendingException(aCx);
    return false;
  }
  nsAutoJSString s;
  if (!s.init(aCx, jsStr)) {
    return false;
  }
  CopyUTF16toUTF8(s, aOut);
  return true;
}

inline bool GetPropU32(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, uint32_t& aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v) || !v.isNumber()) {
    return false;
  }
  aOut = static_cast<uint32_t>(v.toNumber());
  return true;
}

inline bool GetPropU64(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, uint64_t& aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v) || !v.isNumber()) {
    return false;
  }
  aOut = static_cast<uint64_t>(v.toNumber());
  return true;
}

inline bool GetPropI32(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, int32_t& aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v) || !v.isNumber()) {
    return false;
  }
  aOut = static_cast<int32_t>(v.toNumber());
  return true;
}

inline bool GetPropDouble(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, double& aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v) || !v.isNumber()) {
    return false;
  }
  aOut = v.toNumber();
  return true;
}

inline bool GetPropFloat(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, float& aOut) {
  double d = 0;
  if (!GetPropDouble(aCx, aObj, aName, d)) return false;
  aOut = static_cast<float>(d);
  return true;
}

inline bool GetPropBool(JSContext* aCx, JS::Handle<JSObject*> aObj, const char* aName, bool& aOut) {
  JS::Rooted<JS::Value> v(aCx);
  if (!GetPropVal(aCx, aObj, aName, &v)) {
    return false;
  }
  aOut = JS::ToBoolean(v);
  return true;
}

inline WGPUExtent3D ParseExtent3D(JSContext* aCx, JS::Handle<JS::Value> aVal) {
  WGPUExtent3D ext = {1, 1, 1};
  if (!aVal.isObject()) return ext;
  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());
  bool isArr = false;
  if (JS_IsArrayObject(aCx, obj, &isArr) && isArr) {
    uint32_t len = 0;
    JS_GetArrayLength(aCx, obj, &len);
    JS::Rooted<JS::Value> e(aCx);
    if (len > 0 && JS_GetElement(aCx, obj, 0, &e) && e.isNumber()) ext.width = static_cast<uint32_t>(e.toNumber());
    if (len > 1 && JS_GetElement(aCx, obj, 1, &e) && e.isNumber()) ext.height = static_cast<uint32_t>(e.toNumber());
    if (len > 2 && JS_GetElement(aCx, obj, 2, &e) && e.isNumber()) ext.depthOrArrayLayers = static_cast<uint32_t>(e.toNumber());
  } else {
    GetPropU32(aCx, obj, "width", ext.width);
    GetPropU32(aCx, obj, "height", ext.height);
    GetPropU32(aCx, obj, "depthOrArrayLayers", ext.depthOrArrayLayers);
  }
  return ext;
}

inline WGPUOrigin3D ParseOrigin3D(JSContext* aCx, JS::Handle<JS::Value> aVal) {
  WGPUOrigin3D o = {0, 0, 0};
  if (!aVal.isObject()) return o;
  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());
  bool isArr = false;
  if (JS_IsArrayObject(aCx, obj, &isArr) && isArr) {
    uint32_t len = 0;
    JS_GetArrayLength(aCx, obj, &len);
    JS::Rooted<JS::Value> e(aCx);
    if (len > 0 && JS_GetElement(aCx, obj, 0, &e) && e.isNumber()) o.x = static_cast<uint32_t>(e.toNumber());
    if (len > 1 && JS_GetElement(aCx, obj, 1, &e) && e.isNumber()) o.y = static_cast<uint32_t>(e.toNumber());
    if (len > 2 && JS_GetElement(aCx, obj, 2, &e) && e.isNumber()) o.z = static_cast<uint32_t>(e.toNumber());
  } else {
    GetPropU32(aCx, obj, "x", o.x);
    GetPropU32(aCx, obj, "y", o.y);
    GetPropU32(aCx, obj, "z", o.z);
  }
  return o;
}

inline WGPUColor ParseColor(JSContext* aCx, JS::Handle<JS::Value> aVal) {
  WGPUColor c = {0.0, 0.0, 0.0, 1.0};
  if (!aVal.isObject()) return c;
  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());
  bool isArr = false;
  if (JS_IsArrayObject(aCx, obj, &isArr) && isArr) {
    uint32_t len = 0;
    JS_GetArrayLength(aCx, obj, &len);
    JS::Rooted<JS::Value> e(aCx);
    if (len > 0 && JS_GetElement(aCx, obj, 0, &e) && e.isNumber()) c.r = e.toNumber();
    if (len > 1 && JS_GetElement(aCx, obj, 1, &e) && e.isNumber()) c.g = e.toNumber();
    if (len > 2 && JS_GetElement(aCx, obj, 2, &e) && e.isNumber()) c.b = e.toNumber();
    if (len > 3 && JS_GetElement(aCx, obj, 3, &e) && e.isNumber()) c.a = e.toNumber();
  } else {
    GetPropDouble(aCx, obj, "r", c.r);
    GetPropDouble(aCx, obj, "g", c.g);
    GetPropDouble(aCx, obj, "b", c.b);
    GetPropDouble(aCx, obj, "a", c.a);
  }
  return c;
}

inline WGPUTextureFormat ParseTextureFormat(const nsACString& s) {
  if (s.EqualsLiteral("r8unorm")) return WGPUTextureFormat_R8Unorm;
  if (s.EqualsLiteral("r8snorm")) return WGPUTextureFormat_R8Snorm;
  if (s.EqualsLiteral("r8uint")) return WGPUTextureFormat_R8Uint;
  if (s.EqualsLiteral("r8sint")) return WGPUTextureFormat_R8Sint;
  if (s.EqualsLiteral("r16uint")) return WGPUTextureFormat_R16Uint;
  if (s.EqualsLiteral("r16sint")) return WGPUTextureFormat_R16Sint;
  if (s.EqualsLiteral("r16float")) return WGPUTextureFormat_R16Float;
  if (s.EqualsLiteral("rg8unorm")) return WGPUTextureFormat_RG8Unorm;
  if (s.EqualsLiteral("rg8snorm")) return WGPUTextureFormat_RG8Snorm;
  if (s.EqualsLiteral("rg8uint")) return WGPUTextureFormat_RG8Uint;
  if (s.EqualsLiteral("rg8sint")) return WGPUTextureFormat_RG8Sint;
  if (s.EqualsLiteral("r32float")) return WGPUTextureFormat_R32Float;
  if (s.EqualsLiteral("r32uint")) return WGPUTextureFormat_R32Uint;
  if (s.EqualsLiteral("r32sint")) return WGPUTextureFormat_R32Sint;
  if (s.EqualsLiteral("rg16uint")) return WGPUTextureFormat_RG16Uint;
  if (s.EqualsLiteral("rg16sint")) return WGPUTextureFormat_RG16Sint;
  if (s.EqualsLiteral("rg16float")) return WGPUTextureFormat_RG16Float;
  if (s.EqualsLiteral("rgba8unorm")) return WGPUTextureFormat_RGBA8Unorm;
  if (s.EqualsLiteral("rgba8unorm-srgb")) return WGPUTextureFormat_RGBA8UnormSrgb;
  if (s.EqualsLiteral("rgba8snorm")) return WGPUTextureFormat_RGBA8Snorm;
  if (s.EqualsLiteral("rgba8uint")) return WGPUTextureFormat_RGBA8Uint;
  if (s.EqualsLiteral("rgba8sint")) return WGPUTextureFormat_RGBA8Sint;
  if (s.EqualsLiteral("bgra8unorm")) return WGPUTextureFormat_BGRA8Unorm;
  if (s.EqualsLiteral("bgra8unorm-srgb")) return WGPUTextureFormat_BGRA8UnormSrgb;
  if (s.EqualsLiteral("rgb10a2uint")) return WGPUTextureFormat_RGB10A2Uint;
  if (s.EqualsLiteral("rgb10a2unorm")) return WGPUTextureFormat_RGB10A2Unorm;
  if (s.EqualsLiteral("rg11b10ufloat")) return WGPUTextureFormat_RG11B10Ufloat;
  if (s.EqualsLiteral("rgb9e5ufloat")) return WGPUTextureFormat_RGB9E5Ufloat;
  if (s.EqualsLiteral("rg32float")) return WGPUTextureFormat_RG32Float;
  if (s.EqualsLiteral("rg32uint")) return WGPUTextureFormat_RG32Uint;
  if (s.EqualsLiteral("rg32sint")) return WGPUTextureFormat_RG32Sint;
  if (s.EqualsLiteral("rgba16uint")) return WGPUTextureFormat_RGBA16Uint;
  if (s.EqualsLiteral("rgba16sint")) return WGPUTextureFormat_RGBA16Sint;
  if (s.EqualsLiteral("rgba16float")) return WGPUTextureFormat_RGBA16Float;
  if (s.EqualsLiteral("rgba32float")) return WGPUTextureFormat_RGBA32Float;
  if (s.EqualsLiteral("rgba32uint")) return WGPUTextureFormat_RGBA32Uint;
  if (s.EqualsLiteral("rgba32sint")) return WGPUTextureFormat_RGBA32Sint;
  if (s.EqualsLiteral("stencil8")) return WGPUTextureFormat_Stencil8;
  if (s.EqualsLiteral("depth16unorm")) return WGPUTextureFormat_Depth16Unorm;
  if (s.EqualsLiteral("depth24plus")) return WGPUTextureFormat_Depth24Plus;
  if (s.EqualsLiteral("depth24plus-stencil8")) return WGPUTextureFormat_Depth24PlusStencil8;
  if (s.EqualsLiteral("depth32float")) return WGPUTextureFormat_Depth32Float;
  if (s.EqualsLiteral("depth32float-stencil8")) return WGPUTextureFormat_Depth32FloatStencil8;
  if (s.EqualsLiteral("bc1-rgba-unorm")) return WGPUTextureFormat_BC1RGBAUnorm;
  if (s.EqualsLiteral("bc1-rgba-unorm-srgb")) return WGPUTextureFormat_BC1RGBAUnormSrgb;
  if (s.EqualsLiteral("bc2-rgba-unorm")) return WGPUTextureFormat_BC2RGBAUnorm;
  if (s.EqualsLiteral("bc2-rgba-unorm-srgb")) return WGPUTextureFormat_BC2RGBAUnormSrgb;
  if (s.EqualsLiteral("bc3-rgba-unorm")) return WGPUTextureFormat_BC3RGBAUnorm;
  if (s.EqualsLiteral("bc3-rgba-unorm-srgb")) return WGPUTextureFormat_BC3RGBAUnormSrgb;
  if (s.EqualsLiteral("bc4-r-unorm")) return WGPUTextureFormat_BC4RUnorm;
  if (s.EqualsLiteral("bc4-r-snorm")) return WGPUTextureFormat_BC4RSnorm;
  if (s.EqualsLiteral("bc5-rg-unorm")) return WGPUTextureFormat_BC5RGUnorm;
  if (s.EqualsLiteral("bc5-rg-snorm")) return WGPUTextureFormat_BC5RGSnorm;
  if (s.EqualsLiteral("bc6h-rgb-ufloat")) return WGPUTextureFormat_BC6HRGBUfloat;
  if (s.EqualsLiteral("bc6h-rgb-float")) return WGPUTextureFormat_BC6HRGBFloat;
  if (s.EqualsLiteral("bc7-rgba-unorm")) return WGPUTextureFormat_BC7RGBAUnorm;
  if (s.EqualsLiteral("bc7-rgba-unorm-srgb")) return WGPUTextureFormat_BC7RGBAUnormSrgb;
  return WGPUTextureFormat_Undefined;
}

inline WGPUVertexFormat ParseVertexFormat(const nsACString& s) {
  if (s.EqualsLiteral("uint8x2")) return WGPUVertexFormat_Uint8x2;
  if (s.EqualsLiteral("uint8x4")) return WGPUVertexFormat_Uint8x4;
  if (s.EqualsLiteral("sint8x2")) return WGPUVertexFormat_Sint8x2;
  if (s.EqualsLiteral("sint8x4")) return WGPUVertexFormat_Sint8x4;
  if (s.EqualsLiteral("unorm8x2")) return WGPUVertexFormat_Unorm8x2;
  if (s.EqualsLiteral("unorm8x4")) return WGPUVertexFormat_Unorm8x4;
  if (s.EqualsLiteral("snorm8x2")) return WGPUVertexFormat_Snorm8x2;
  if (s.EqualsLiteral("snorm8x4")) return WGPUVertexFormat_Snorm8x4;
  if (s.EqualsLiteral("uint16x2")) return WGPUVertexFormat_Uint16x2;
  if (s.EqualsLiteral("uint16x4")) return WGPUVertexFormat_Uint16x4;
  if (s.EqualsLiteral("sint16x2")) return WGPUVertexFormat_Sint16x2;
  if (s.EqualsLiteral("sint16x4")) return WGPUVertexFormat_Sint16x4;
  if (s.EqualsLiteral("unorm16x2")) return WGPUVertexFormat_Unorm16x2;
  if (s.EqualsLiteral("unorm16x4")) return WGPUVertexFormat_Unorm16x4;
  if (s.EqualsLiteral("snorm16x2")) return WGPUVertexFormat_Snorm16x2;
  if (s.EqualsLiteral("snorm16x4")) return WGPUVertexFormat_Snorm16x4;
  if (s.EqualsLiteral("float16x2")) return WGPUVertexFormat_Float16x2;
  if (s.EqualsLiteral("float16x4")) return WGPUVertexFormat_Float16x4;
  if (s.EqualsLiteral("float32")) return WGPUVertexFormat_Float32;
  if (s.EqualsLiteral("float32x2")) return WGPUVertexFormat_Float32x2;
  if (s.EqualsLiteral("float32x3")) return WGPUVertexFormat_Float32x3;
  if (s.EqualsLiteral("float32x4")) return WGPUVertexFormat_Float32x4;
  if (s.EqualsLiteral("uint32")) return WGPUVertexFormat_Uint32;
  if (s.EqualsLiteral("uint32x2")) return WGPUVertexFormat_Uint32x2;
  if (s.EqualsLiteral("uint32x3")) return WGPUVertexFormat_Uint32x3;
  if (s.EqualsLiteral("uint32x4")) return WGPUVertexFormat_Uint32x4;
  if (s.EqualsLiteral("sint32")) return WGPUVertexFormat_Sint32;
  if (s.EqualsLiteral("sint32x2")) return WGPUVertexFormat_Sint32x2;
  if (s.EqualsLiteral("sint32x3")) return WGPUVertexFormat_Sint32x3;
  if (s.EqualsLiteral("sint32x4")) return WGPUVertexFormat_Sint32x4;
  return WGPUVertexFormat_Float32x4;
}

inline WGPUBlendFactor ParseBlendFactor(const nsACString& s) {
  if (s.EqualsLiteral("zero")) return WGPUBlendFactor_Zero;
  if (s.EqualsLiteral("one")) return WGPUBlendFactor_One;
  if (s.EqualsLiteral("src")) return WGPUBlendFactor_Src;
  if (s.EqualsLiteral("one-minus-src")) return WGPUBlendFactor_OneMinusSrc;
  if (s.EqualsLiteral("src-alpha")) return WGPUBlendFactor_SrcAlpha;
  if (s.EqualsLiteral("one-minus-src-alpha")) return WGPUBlendFactor_OneMinusSrcAlpha;
  if (s.EqualsLiteral("dst")) return WGPUBlendFactor_Dst;
  if (s.EqualsLiteral("one-minus-dst")) return WGPUBlendFactor_OneMinusDst;
  if (s.EqualsLiteral("dst-alpha")) return WGPUBlendFactor_DstAlpha;
  if (s.EqualsLiteral("one-minus-dst-alpha")) return WGPUBlendFactor_OneMinusDstAlpha;
  if (s.EqualsLiteral("src-alpha-saturated")) return WGPUBlendFactor_SrcAlphaSaturated;
  if (s.EqualsLiteral("constant")) return WGPUBlendFactor_Constant;
  if (s.EqualsLiteral("one-minus-constant")) return WGPUBlendFactor_OneMinusConstant;
  return WGPUBlendFactor_One;
}

inline WGPUBlendOperation ParseBlendOp(const nsACString& s) {
  if (s.EqualsLiteral("add")) return WGPUBlendOperation_Add;
  if (s.EqualsLiteral("subtract")) return WGPUBlendOperation_Subtract;
  if (s.EqualsLiteral("reverse-subtract")) return WGPUBlendOperation_ReverseSubtract;
  if (s.EqualsLiteral("min")) return WGPUBlendOperation_Min;
  if (s.EqualsLiteral("max")) return WGPUBlendOperation_Max;
  return WGPUBlendOperation_Add;
}

inline WGPUCompareFunction ParseCompareFunc(const nsACString& s) {
  if (s.EqualsLiteral("never")) return WGPUCompareFunction_Never;
  if (s.EqualsLiteral("less")) return WGPUCompareFunction_Less;
  if (s.EqualsLiteral("equal")) return WGPUCompareFunction_Equal;
  if (s.EqualsLiteral("less-equal")) return WGPUCompareFunction_LessEqual;
  if (s.EqualsLiteral("greater")) return WGPUCompareFunction_Greater;
  if (s.EqualsLiteral("not-equal")) return WGPUCompareFunction_NotEqual;
  if (s.EqualsLiteral("greater-equal")) return WGPUCompareFunction_GreaterEqual;
  if (s.EqualsLiteral("always")) return WGPUCompareFunction_Always;
  return WGPUCompareFunction_Undefined;
}

inline WGPUStencilOperation ParseStencilOp(const nsACString& s) {
  if (s.EqualsLiteral("keep")) return WGPUStencilOperation_Keep;
  if (s.EqualsLiteral("zero")) return WGPUStencilOperation_Zero;
  if (s.EqualsLiteral("replace")) return WGPUStencilOperation_Replace;
  if (s.EqualsLiteral("invert")) return WGPUStencilOperation_Invert;
  if (s.EqualsLiteral("increment-clamp")) return WGPUStencilOperation_IncrementClamp;
  if (s.EqualsLiteral("decrement-clamp")) return WGPUStencilOperation_DecrementClamp;
  if (s.EqualsLiteral("increment-wrap")) return WGPUStencilOperation_IncrementWrap;
  if (s.EqualsLiteral("decrement-wrap")) return WGPUStencilOperation_DecrementWrap;
  return WGPUStencilOperation_Keep;
}

inline WGPUPrimitiveTopology ParseTopology(const nsACString& s) {
  if (s.EqualsLiteral("point-list")) return WGPUPrimitiveTopology_PointList;
  if (s.EqualsLiteral("line-list")) return WGPUPrimitiveTopology_LineList;
  if (s.EqualsLiteral("line-strip")) return WGPUPrimitiveTopology_LineStrip;
  if (s.EqualsLiteral("triangle-list")) return WGPUPrimitiveTopology_TriangleList;
  if (s.EqualsLiteral("triangle-strip")) return WGPUPrimitiveTopology_TriangleStrip;
  return WGPUPrimitiveTopology_TriangleList;
}

inline WGPUCullMode ParseCullMode(const nsACString& s) {
  if (s.EqualsLiteral("front")) return WGPUCullMode_Front;
  if (s.EqualsLiteral("back")) return WGPUCullMode_Back;
  return WGPUCullMode_None;
}

inline WGPUFrontFace ParseFrontFace(const nsACString& s) {
  if (s.EqualsLiteral("cw")) return WGPUFrontFace_CW;
  return WGPUFrontFace_CCW;
}

inline WGPUIndexFormat ParseIndexFormat(const nsACString& s) {
  if (s.EqualsLiteral("uint16")) return WGPUIndexFormat_Uint16;
  if (s.EqualsLiteral("uint32")) return WGPUIndexFormat_Uint32;
  return WGPUIndexFormat_Undefined;
}

inline WGPULoadOp ParseLoadOp(const nsACString& s) {
  if (s.EqualsLiteral("clear")) return WGPULoadOp_Clear;
  if (s.EqualsLiteral("load")) return WGPULoadOp_Load;
  return WGPULoadOp_Undefined;
}

inline WGPUStoreOp ParseStoreOp(const nsACString& s) {
  if (s.EqualsLiteral("store")) return WGPUStoreOp_Store;
  if (s.EqualsLiteral("discard")) return WGPUStoreOp_Discard;
  return WGPUStoreOp_Undefined;
}

inline WGPUAddressMode ParseAddressMode(const nsACString& s) {
  if (s.EqualsLiteral("repeat")) return WGPUAddressMode_Repeat;
  if (s.EqualsLiteral("mirror-repeat")) return WGPUAddressMode_MirrorRepeat;
  return WGPUAddressMode_ClampToEdge;
}

inline WGPUFilterMode ParseFilterMode(const nsACString& s) {
  if (s.EqualsLiteral("linear")) return WGPUFilterMode_Linear;
  return WGPUFilterMode_Nearest;
}

inline WGPUMipmapFilterMode ParseMipmapFilterMode(const nsACString& s) {
  if (s.EqualsLiteral("linear")) return WGPUMipmapFilterMode_Linear;
  return WGPUMipmapFilterMode_Nearest;
}

inline WGPUTextureDimension ParseTextureDimension(const nsACString& s) {
  if (s.EqualsLiteral("1d")) return WGPUTextureDimension_1D;
  if (s.EqualsLiteral("3d")) return WGPUTextureDimension_3D;
  return WGPUTextureDimension_2D;
}

inline WGPUTextureViewDimension ParseTextureViewDimension(const nsACString& s) {
  if (s.EqualsLiteral("1d")) return WGPUTextureViewDimension_1D;
  if (s.EqualsLiteral("2d")) return WGPUTextureViewDimension_2D;
  if (s.EqualsLiteral("2d-array")) return WGPUTextureViewDimension_2DArray;
  if (s.EqualsLiteral("cube")) return WGPUTextureViewDimension_Cube;
  if (s.EqualsLiteral("cube-array")) return WGPUTextureViewDimension_CubeArray;
  if (s.EqualsLiteral("3d")) return WGPUTextureViewDimension_3D;
  return WGPUTextureViewDimension_Undefined;
}

inline WGPUTextureAspect ParseTextureAspect(const nsACString& s) {
  if (s.EqualsLiteral("stencil-only")) return WGPUTextureAspect_StencilOnly;
  if (s.EqualsLiteral("depth-only")) return WGPUTextureAspect_DepthOnly;
  return WGPUTextureAspect_All;
}

inline WGPUBufferBindingType ParseBufferBindingType(const nsACString& s) {
  if (s.EqualsLiteral("uniform")) return WGPUBufferBindingType_Uniform;
  if (s.EqualsLiteral("storage")) return WGPUBufferBindingType_Storage;
  if (s.EqualsLiteral("read-only-storage")) return WGPUBufferBindingType_ReadOnlyStorage;
  return WGPUBufferBindingType_Uniform;
}

inline WGPUSamplerBindingType ParseSamplerBindingType(const nsACString& s) {
  if (s.EqualsLiteral("filtering")) return WGPUSamplerBindingType_Filtering;
  if (s.EqualsLiteral("non-filtering")) return WGPUSamplerBindingType_NonFiltering;
  if (s.EqualsLiteral("comparison")) return WGPUSamplerBindingType_Comparison;
  return WGPUSamplerBindingType_Filtering;
}

inline WGPUTextureSampleType ParseTextureSampleType(const nsACString& s) {
  if (s.EqualsLiteral("float")) return WGPUTextureSampleType_Float;
  if (s.EqualsLiteral("unfilterable-float")) return WGPUTextureSampleType_UnfilterableFloat;
  if (s.EqualsLiteral("depth")) return WGPUTextureSampleType_Depth;
  if (s.EqualsLiteral("sint")) return WGPUTextureSampleType_Sint;
  if (s.EqualsLiteral("uint")) return WGPUTextureSampleType_Uint;
  return WGPUTextureSampleType_Float;
}

inline WGPUStorageTextureAccess ParseStorageTextureAccess(const nsACString& s) {
  if (s.EqualsLiteral("write-only")) return WGPUStorageTextureAccess_WriteOnly;
  if (s.EqualsLiteral("read-only")) return WGPUStorageTextureAccess_ReadOnly;
  if (s.EqualsLiteral("read-write")) return WGPUStorageTextureAccess_ReadWrite;
  return WGPUStorageTextureAccess_WriteOnly;
}

inline WGPUVertexStepMode ParseStepMode(const nsACString& s) {
  if (s.EqualsLiteral("instance")) return WGPUVertexStepMode_Instance;
  return WGPUVertexStepMode_Vertex;
}

inline WGPUFeatureName ParseFeatureName(const nsACString& s) {
  if (s.EqualsLiteral("depth-clip-control")) return WGPUFeatureName_DepthClipControl;
  if (s.EqualsLiteral("depth32float-stencil8")) return WGPUFeatureName_Depth32FloatStencil8;
  if (s.EqualsLiteral("timestamp-query")) return WGPUFeatureName_TimestampQuery;
  if (s.EqualsLiteral("texture-compression-bc")) return WGPUFeatureName_TextureCompressionBC;
  if (s.EqualsLiteral("texture-compression-etc2")) return WGPUFeatureName_TextureCompressionETC2;
  if (s.EqualsLiteral("texture-compression-astc")) return WGPUFeatureName_TextureCompressionASTC;
  if (s.EqualsLiteral("indirect-first-instance")) return WGPUFeatureName_IndirectFirstInstance;
  if (s.EqualsLiteral("shader-f16")) return WGPUFeatureName_ShaderF16;
  if (s.EqualsLiteral("rg11b10ufloat-renderable")) return WGPUFeatureName_RG11B10UfloatRenderable;
  if (s.EqualsLiteral("bgra8unorm-storage")) return WGPUFeatureName_BGRA8UnormStorage;
  if (s.EqualsLiteral("float32-filterable")) return WGPUFeatureName_Float32Filterable;
  return static_cast<WGPUFeatureName>(0);
}

inline const char* FeatureNameToString(WGPUFeatureName f) {
  switch (f) {
    case WGPUFeatureName_DepthClipControl: return "depth-clip-control";
    case WGPUFeatureName_Depth32FloatStencil8: return "depth32float-stencil8";
    case WGPUFeatureName_TimestampQuery: return "timestamp-query";
    case WGPUFeatureName_TextureCompressionBC: return "texture-compression-bc";
    case WGPUFeatureName_TextureCompressionETC2: return "texture-compression-etc2";
    case WGPUFeatureName_TextureCompressionASTC: return "texture-compression-astc";
    case WGPUFeatureName_IndirectFirstInstance: return "indirect-first-instance";
    case WGPUFeatureName_ShaderF16: return "shader-f16";
    case WGPUFeatureName_RG11B10UfloatRenderable: return "rg11b10ufloat-renderable";
    case WGPUFeatureName_BGRA8UnormStorage: return "bgra8unorm-storage";
    case WGPUFeatureName_Float32Filterable: return "float32-filterable";
    default: return nullptr;
  }
}

} // namespace webgpu_helpers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_WebGPUHelpers_h
