#include "glass.h"

#include <imgui.h>
#include <d3dcompiler.h>
#include <cmath>
#include <string>
#include <cstring>
#include <cstdio>

// 临时崩溃定位日志（排查后移除）
static void GLog(const char* tag) {
    if (FILE* f = std::fopen("C:\\Users\\ZhuanZ1\\AppData\\Local\\Temp\\sfsui_debug.log", "a")) {
        std::fputs(tag, f);
        std::fputs("\n", f);
        std::fclose(f);
    }
}

// ---------------------------------------------------------------------------
// HLSL
// ---------------------------------------------------------------------------
static const char* kVS = R"HLSL(
float4 main(float2 pos : POSITION) : SV_POSITION {
    return float4(pos, 0.0, 1.0);
}
)HLSL";

// 通用 CB：渐变 / 模糊 / 复制共用（float4 对齐）
struct PassCb {
    float size[2];
    float time;
    float pad0;
    float dir[2];      // 模糊方向
    float texel[2];    // 源纹理纹素
};

static const char* kPSGradient = R"HLSL(
cbuffer Cb : register(b0) {
    float2 gSize;
    float  gTime;
    float  gPad;
    float2 gDir;
    float2 gTexel;
}

float hash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float4 main(float4 pix : SV_POSITION) : SV_Target {
    float2 uv = pix.xy / gSize;
    float2 c = uv - 0.5;
    c.x *= gSize.x / gSize.y;

    float3 col = float3(0.043, 0.055, 0.078);           // 深夜底色
    float t = gTime;

    // 三个缓慢漂移的辉光团（品牌青 / 靛蓝 / 暖紫）
    float2 p1 = float2(-0.55 + 0.35 * sin(t * 0.21), -0.30 + 0.22 * cos(t * 0.17));
    float2 p2 = float2( 0.60 + 0.30 * cos(t * 0.13 + 2.1), 0.35 + 0.30 * sin(t * 0.19 + 1.3));
    float2 p3 = float2( 0.10 + 0.45 * sin(t * 0.11 + 4.2), -0.45 + 0.25 * cos(t * 0.23 + 0.6));
    col += float3(0.05, 0.30, 0.26) * exp(-dot(c - p1, c - p1) * 3.2);
    col += float3(0.16, 0.10, 0.34) * exp(-dot(c - p2, c - p2) * 2.6);
    col += float3(0.28, 0.12, 0.22) * exp(-dot(c - p3, c - p3) * 3.8);

    // 暗角
    col *= 1.0 - 0.55 * dot(c, c);

    // 抖动去色带
    col += (hash(pix.xy) - 0.5) * (1.5 / 255.0);
    return float4(col, 1.0);
}
)HLSL";

// 可分离高斯（dir=0 时退化为下采样复制）
static const char* kPSBlur = R"HLSL(
cbuffer Cb : register(b0) {
    float2 gSize;
    float  gTime;
    float  gPad;
    float2 gDir;
    float2 gTexel;
}
SamplerState sam : register(s0);
Texture2D src : register(t0);

static const float w0 = 0.2270270270;
static const float w1 = 0.3162162162;
static const float w2 = 0.0702702703;

float4 main(float4 pix : SV_POSITION) : SV_Target {
    float2 uv = pix.xy * gTexel;
    float2 dir = gDir * gTexel;
    float3 acc = src.Sample(sam, uv).rgb * w0;
    acc += (src.Sample(sam, uv + dir * 1.3846).rgb +
            src.Sample(sam, uv - dir * 1.3846).rgb) * w1;
    acc += (src.Sample(sam, uv + dir * 3.2308).rgb +
            src.Sample(sam, uv - dir * 3.2308).rgb) * w2;
    return float4(acc, 1.0);
}
)HLSL";

static const char* kPSCopy = R"HLSL(
cbuffer Cb : register(b0) {
    float2 gSize;
    float  gTime;
    float  gPad;
    float2 gDir;
    float2 gTexel;
}
SamplerState sam : register(s0);
Texture2D src : register(t0);
float4 main(float4 pix : SV_POSITION) : SV_Target {
    return src.Sample(sam, pix.xy * gTexel);
}
)HLSL";

// 抓屏纹理（B8G8R8A8）→ RGBA 进 scene
static const char* kPSBgra = R"HLSL(
cbuffer Cb : register(b0) {
    float2 gSize;
    float  gTime;
    float  gPad;
    float2 gDir;
    float2 gTexel;
}
SamplerState sam : register(s0);
Texture2D src : register(t0);
float4 main(float4 pix : SV_POSITION) : SV_Target {
    return src.Sample(sam, pix.xy * gTexel).bgra;
}
)HLSL";

// 玻璃合成 —— 移植自参考实现 liquid_glass.frag + glass_math + glass_lighting
static const char* kPSGlass = R"HLSL(
cbuffer Cb : register(b0) {
    float4 gBounds;        // x0,y0,x1,y1（像素）
    float4 gA;             // size.x, size.y, refractionHeight, refractionAmount
    float4 gB;             // chromatic, tint.r, tint.g, tint.b
    float4 gC;             // tint.a, innerShadow, rimGlow, specular
    float4 gD;             // outerShadow, shadowExtent, specularPower, alpha
    float4 gLight;         // 光照方向 xy
}
SamplerState sam : register(s0);
Texture2D backdrop : register(t0);

float sdRoundedRect(float2 pt, float2 halfSize, float radius) {
    float2 corner = abs(pt) - (halfSize - float2(radius, radius));
    float outside = length(max(corner, float2(0.0, 0.0))) - radius;
    float inside = min(max(corner.x, corner.y), 0.0);
    return outside + inside;
}

float antialiasedCoverage(float signedDistance) {
    float edgeWidth = max(fwidth(signedDistance), 0.5);
    return 1.0 - smoothstep(-edgeWidth, edgeWidth, signedDistance);
}

float safeSign(float value) { return value < 0.0 ? -1.0 : 1.0; }

float2 safeNormalize(float2 value, float2 fallback) {
    float len = length(value);
    return len > 0.001 ? value / len : fallback;
}

float2 gradSdRoundedRect(float2 pt, float2 halfSize, float radius) {
    float2 corner = abs(pt) - (halfSize - float2(radius, radius));
    if (corner.x >= 0.0 || corner.y >= 0.0) {
        float2 outside = max(corner, float2(0.0, 0.0));
        float outsideLength = length(outside);
        if (outsideLength > 0.001)
            return sign(pt) * (outside / outsideLength);
        float useX = step(corner.y, corner.x);
        return float2(useX * safeSign(pt.x), (1.0 - useX) * safeSign(pt.y));
    }
    float useX = step(corner.y, corner.x);
    return sign(pt) * float2(useX, 1.0 - useX);
}

float circleMap(float value) {
    float mapped = clamp(value, -1.0, 1.0);
    return 1.0 - sqrt(max(0.0, 1.0 - mapped * mapped));
}

// --- 光照（glass_lighting.glslinc） ---
float rimIntegral(float t, float h) {
    float u = 1.0 - sqrt(clamp(t / h, 0.0, 1.0));
    float u6 = u * u * u * u * u * u;
    return 2.0 * h * u6 * (u / 7.0 - 1.0 / 6.0);
}

float filteredRim(float signedDistance, float edgeWidth, float2 distanceGradient) {
    float h = max(edgeWidth, 0.001);
    float halfPixelWidth = 0.5 * max(abs(distanceGradient.x) + abs(distanceGradient.y), 1e-4);
    float t0 = max(-signedDistance - halfPixelWidth, 0.0);
    float t1 = max(-signedDistance + halfPixelWidth, 0.0);
    if (t1 <= 0.0) return 0.0;
    if (t1 - t0 < 1e-4) {
        float u = 1.0 - sqrt(clamp(t0 / h, 0.0, 1.0));
        return u * u * u * u * u;
    }
    return (rimIntegral(t1, h) - rimIntegral(t0, h)) / (t1 - t0);
}

float3 applyGlassLighting(float3 color, float signedDistance, float2 normal,
                          float edgeWidth, float2 distanceGradient) {
    float rim = filteredRim(signedDistance, edgeWidth, distanceGradient);
    float2 lightDirection = safeNormalize(gLight.xy, float2(-0.7071, -0.7071));

    float shadowFacing = max(dot(normal, -lightDirection), 0.0);
    color *= 1.0 - gC.y * rim * shadowFacing;                    // innerShadow
    color += gC.z * rim * (float3(1.0, 1.0, 1.0) - color);       // rimGlow

    float specular = pow(max(dot(normal, lightDirection), 0.0), max(gD.z, 1.0));
    color += gC.w * rim * specular * (float3(1.0, 1.0, 1.0) - color);
    return color;
}

float glassOuterShadow(float signedDistance, float2 normal) {
    float extent = max(gD.y, 0.001);
    float falloff = 1.0 - smoothstep(0.0, extent, signedDistance);
    float2 lightDirection = safeNormalize(gLight.xy, float2(-0.7071, -0.7071));
    float direction = max(dot(normal, -lightDirection), 0.0);
    return gD.x * falloff * lerp(0.35, 1.0, direction);
}

// --- 滤色（glass_color.glslinc） ---
float3 toLinearSrgb(float3 color) {
    float3 low = color / 12.92;
    float3 high = pow(max((color + 0.055) / 1.055, float3(0.0, 0.0, 0.0)), float3(2.4, 2.4, 2.4));
    return lerp(high, low, step(color, float3(0.04045, 0.04045, 0.04045)));
}

float3 fromLinearSrgb(float3 color) {
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(max(color, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return lerp(high, low, step(color, float3(0.0031308, 0.0031308, 0.0031308)));
}

float4 sampleContent(float2 pixelCoordinate) {
    float2 halfTexel = 0.5 / gA.xy;
    float2 uv = clamp(pixelCoordinate / gA.xy, halfTexel, 1.0 - halfTexel);
    return backdrop.Sample(sam, uv);
}

float4 filterLiquidGlassColor(float4 color) {
    float3 lin = toLinearSrgb(color.rgb);
    float luminance = dot(lin, float3(0.2126, 0.7152, 0.0722));
    color.rgb = fromLinearSrgb(lerp(float3(luminance, luminance, luminance), lin, 1.0));
    color.rgb = lerp(color.rgb, gB.yzw, gC.x);   // tint 染色
    color.a = gD.w;                              // 整体不透明度（uFilter.w）
    return color;
}

float4 main(float4 pix : SV_POSITION) : SV_Target {
    float2 pixel = pix.xy;
    float2 size = max(gBounds.zw - gBounds.xy, float2(1.0, 1.0));
    float2 halfSize = size * 0.5;
    float2 centered = pixel - (gBounds.xy + halfSize);
    float radius = min(22.0, min(halfSize.x, halfSize.y));
    float signedDistance = sdRoundedRect(centered, halfSize, radius);
    float smoothRadius = min(max(radius * 1.5, 30.0), min(halfSize.x, halfSize.y));
    float2 shapeGradient = gradSdRoundedRect(centered, halfSize, smoothRadius);
    float2 distanceGradient = float2(ddx(signedDistance), ddy(signedDistance));
    float coverage = antialiasedCoverage(signedDistance);

    float shadow = gD.x > 0.0 && signedDistance < gD.y
                       ? glassOuterShadow(max(signedDistance, 0.0), shapeGradient)
                       : 0.0;
    if (coverage <= 0.0) {
        if (shadow <= 0.001) discard;
        return float4(0.0, 0.0, 0.0, shadow * gD.w);
    }

    // 边缘折射 + 七采样色散；内部直采模糊图
    float refractionHeight = max(gA.z, 0.001);
    float shadeDistance = min(signedDistance, -0.001);
    float4 color;
    if (-shadeDistance >= refractionHeight) {
        color = sampleContent(pixel);
    } else {
        float depth = 1.0 - (-shadeDistance / refractionHeight);
        float displacement = circleMap(depth) * gA.w;
        float2 depthGradient = safeNormalize(centered, shapeGradient);
        float2 gradient = safeNormalize(shapeGradient + 0.3 * depthGradient, shapeGradient);
        float2 refracted = pixel + displacement * gradient;
        float dispersionIntensity = gB.x *
            ((centered.x * centered.y) / max(halfSize.x * halfSize.y, 0.001));
        float2 dispersed = displacement * gradient * dispersionIntensity;

        color = float4(0.0, 0.0, 0.0, 0.0);
        float4 red    = sampleContent(refracted + dispersed);
        color.r += red.r / 3.5;    color.a += red.a / 7.0;
        float4 orange = sampleContent(refracted + dispersed * (2.0 / 3.0));
        color.r += orange.r / 3.5; color.g += orange.g / 7.0; color.a += orange.a / 7.0;
        float4 yellow = sampleContent(refracted + dispersed * (1.0 / 3.0));
        color.r += yellow.r / 3.5; color.g += yellow.g / 3.5; color.a += yellow.a / 7.0;
        float4 green  = sampleContent(refracted);
        color.g += green.g / 3.5;  color.a += green.a / 7.0;
        float4 cyan   = sampleContent(refracted - dispersed * (1.0 / 3.0));
        color.g += cyan.g / 3.5;   color.b += cyan.b / 3.0;   color.a += cyan.a / 7.0;
        float4 blue   = sampleContent(refracted - dispersed * (2.0 / 3.0));
        color.b += blue.b / 3.0;   color.a += blue.a / 7.0;
        float4 purple = sampleContent(refracted - dispersed);
        color.r += purple.r / 7.0; color.b += purple.b / 3.0; color.a += purple.a / 7.0;
    }

    color = filterLiquidGlassColor(color);
    // 磨砂玻璃质感：轻微提亮+抬黑，让模糊背景比真实桌面更"粉"一点（参考图观感）
    color.rgb = color.rgb * 1.12 + 0.028;
    color.rgb = applyGlassLighting(color.rgb, signedDistance, shapeGradient,
                                   refractionHeight, distanceGradient);
    float glassAlpha = color.a * coverage;
    float alpha = glassAlpha + shadow * gD.w * (1.0 - coverage);
    return float4(color.rgb * (glassAlpha / max(alpha, 1e-6)), alpha);
}
)HLSL";

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static HRESULT Compile(const char* src, const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (err) {
        GLog((std::string("compile[") + entry + "/" + target + "]: "
              + (const char*)err->GetBufferPointer()).c_str());
        err->Release();
    }
    return hr;
}

template <typename T>
static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool GlassRenderer::Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) {
    ready_ = false;
    dev_ = device;
    ctx_ = context;
    width_ = width;
    height_ = height;

    // 全屏三角形带
    float quad[] = {-1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{quad, 0, 0};
    if (FAILED(dev_->CreateBuffer(&bd, &init, &quad_vb_))) return false;

    ID3DBlob* vs_blob = nullptr;
    if (FAILED(Compile(kVS, "main", "vs_5_0", &vs_blob))) return false;
    D3D11_INPUT_ELEMENT_DESC ied[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    HRESULT hr = dev_->CreateInputLayout(ied, 1, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout_);
    hr = dev_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_);
    vs_blob->Release();
    if (FAILED(hr)) return false;

    auto make_ps = [&](const char* src, ID3D11PixelShader** ps) {
        ID3DBlob* blob = nullptr;
        if (FAILED(Compile(src, "main", "ps_5_0", &blob))) return false;
        HRESULT r = dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ps);
        blob->Release();
        return SUCCEEDED(r);
    };
    if (!make_ps(kPSGradient, &ps_gradient_)) return false;
    if (!make_ps(kPSBlur, &ps_blur_)) return false;
    if (!make_ps(kPSCopy, &ps_copy_)) return false;
    if (!make_ps(kPSGlass, &ps_glass_)) return false;
    if (!make_ps(kPSBgra, &ps_bgra_)) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev_->CreateSamplerState(&sd, &sampler_))) return false;

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev_->CreateBlendState(&bld, &blend_premul_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev_->CreateRasterizerState(&rd, &rs_))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(PassCb);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev_->CreateBuffer(&cbd, nullptr, &cb_common_))) return false;
    cbd.ByteWidth = 96;   // 玻璃 CB：6 x float4
    if (FAILED(dev_->CreateBuffer(&cbd, nullptr, &cb_glass_))) return false;

    if (!CreateRenderTargets(width, height)) return false;
    ready_ = true;
    return true;
}

bool GlassRenderer::CreateRenderTargets(int width, int height) {
    Release(scene_rtv_); Release(scene_srv_); Release(scene_tex_);
    Release(blur_a_rtv_); Release(blur_a_srv_); Release(blur_a_tex_);
    Release(blur_b_rtv_); Release(blur_b_srv_); Release(blur_b_tex_);
    if (width <= 0 || height <= 0) return true;

    auto make_rt = [&](int w, int h, ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, tex))) return false;
        if (rtv && FAILED(dev_->CreateRenderTargetView(*tex, nullptr, rtv))) return false;
        if (srv && FAILED(dev_->CreateShaderResourceView(*tex, nullptr, srv))) return false;
        return true;
    };
    if (!make_rt(width, height, &scene_tex_, &scene_rtv_, &scene_srv_)) return false;
    int bw = width / 4, bh = height / 4;
    if (!make_rt(bw, bh, &blur_a_tex_, &blur_a_rtv_, &blur_a_srv_)) return false;
    if (!make_rt(bw, bh, &blur_b_tex_, &blur_b_rtv_, &blur_b_srv_)) return false;
    if (mode_ == GLASS_ACRYLIC) {
        // 亚克力模式下模糊图是面板的"背景"，重建后必须清成全透明
        float z[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(blur_a_rtv_, z);
        ctx_->ClearRenderTargetView(blur_b_rtv_, z);
    }
    return true;
}

void GlassRenderer::SetMode(int mode) {
    mode_ = mode;
    if (ready_ && mode_ == GLASS_ACRYLIC) {
        // 亚克力模式下模糊图不再绘制，清成全透明（面板采样得到纯染色+光照）
        if (blur_a_rtv_) { float z[4] = {0, 0, 0, 0}; ctx_->ClearRenderTargetView(blur_a_rtv_, z); }
        if (blur_b_rtv_) { float z[4] = {0, 0, 0, 0}; ctx_->ClearRenderTargetView(blur_b_rtv_, z); }
    }
}

void GlassRenderer::Resize(int width, int height) {
    if (!ready_) return;   // Init 未成功前不创建任何资源
    width_ = width;
    height_ = height;
    CreateRenderTargets(width, height);
}

void GlassRenderer::DrawPass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv,
                             ID3D11ShaderResourceView* srv, float vp_w, float vp_h,
                             float time, float dirx, float diry, float texelx, float texely) {
    PassCb cb{{vp_w, vp_h}, time, 0.f, {dirx, diry}, {texelx, texely}};
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx_->Map(cb_common_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    memcpy(map.pData, &cb, sizeof(cb));
    ctx_->Unmap(cb_common_, 0);

    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.f, 0.f, vp_w, vp_h, 0.f, 1.f};
    ctx_->RSSetViewports(1, &vp);

    UINT stride = 8, offset = 0;
    ctx_->IASetInputLayout(input_layout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->IASetVertexBuffers(0, 1, &quad_vb_, &stride, &offset);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb_common_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->Draw(4, 0);
}

void GlassRenderer::EnsureCaptureSize(int w, int h) {
    if (cap_w_ == w && cap_h_ == h && cap_tex_) return;
    Release(cap_tex_); Release(cap_srv_); Release(cap_staging_);
    if (cap_bitmap_) { DeleteObject(cap_bitmap_); cap_bitmap_ = nullptr; cap_bits_ = nullptr; }
    cap_w_ = cap_h_ = 0;
    if (w <= 0 || h <= 0) return;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &cap_tex_))) return;
    if (FAILED(dev_->CreateShaderResourceView(cap_tex_, nullptr, &cap_srv_))) return;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &cap_staging_))) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    cap_bitmap_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &cap_bits_, nullptr, 0);
    cap_w_ = w; cap_h_ = h;
}

void GlassRenderer::CaptureBackdrop(HWND hwnd) {
    if (!ready_ || mode_ != GLASS_CAPTURE) return;
    if (single_shot_ && cap_done_) return;
    if (!live_capture_ && cap_done_) return;   // 静止时冻结背景；只有拖动/缩放/首次才抓
    unsigned long long now = GetTickCount64();
    if (now - last_cap_ms_ < 15) return;   // ~60fps，拖动时背景跟随
    last_cap_ms_ = now;

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int vw = GetSystemMetrics(SM_CXSCREEN), vh = GetSystemMetrics(SM_CYSCREEN);
    int x0 = rc.left < 0 ? 0 : rc.left, y0 = rc.top < 0 ? 0 : rc.top;
    int x1 = rc.right > vw ? vw : rc.right, y1 = rc.bottom > vh ? vh : rc.bottom;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    EnsureCaptureSize(w, h);
    if (!cap_bitmap_ || !cap_staging_) return;

    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HGDIOBJ old = SelectObject(mdc, cap_bitmap_);
    BitBlt(mdc, 0, 0, w, h, sdc, x0, y0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(cap_staging_, 0, D3D11_MAP_WRITE, 0, &m))) return;
    const int rowsize = w * 4;
    for (int y = 0; y < h; y++)
        memcpy((BYTE*)m.pData + y * m.RowPitch, (const BYTE*)cap_bits_ + y * rowsize, rowsize);
    ctx_->Unmap(cap_staging_, 0);
    ctx_->CopyResource(cap_tex_, cap_staging_);
    cap_done_ = true;
}

void GlassRenderer::DrawGlassPanel(const GlassRect& p) {
    struct GlassCb {
        float bounds[4];
        float a[4];   // size.x, size.y, refractionHeight, refractionAmount
        float b[4];   // chromatic, tint.rgb
        float c[4];   // tint.a, innerShadow, rimGlow, specular
        float d[4];   // outerShadow, shadowExtent, specularPower, alpha
        float light[4];
    };
    GlassCb cb{};
    cb.bounds[0] = p.x0; cb.bounds[1] = p.y0; cb.bounds[2] = p.x1; cb.bounds[3] = p.y1;
    cb.a[0] = (float)width_; cb.a[1] = (float)height_;
    cb.a[2] = 22.f;      // refractionHeight（参考默认 20）
    cb.a[3] = -55.f;     // refractionAmount（参考默认 -70，桌面收敛一点）
    cb.b[0] = 0.5f;      // chromaticAberration
    cb.b[1] = 0.55f; cb.b[2] = 0.72f; cb.b[3] = 0.70f;   // tint.rgb 微青
    cb.c[0] = 0.03f;     // tint.a：极轻染色
    cb.c[1] = 0.12f;     // innerShadow
    cb.c[2] = 0.20f;     // rimGlow
    cb.c[3] = 0.35f;     // specular
    cb.d[0] = 0.16f;     // outerShadow
    cb.d[1] = 6.f;       // shadowExtent
    cb.d[2] = 28.f;      // specularPower
    cb.d[3] = p.alpha;
    cb.light[0] = -0.7071f; cb.light[1] = -0.7071f;

    D3D11_MAPPED_SUBRESOURCE map{};
    ctx_->Map(cb_glass_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    memcpy(map.pData, &cb, sizeof(cb));
    ctx_->Unmap(cb_glass_, 0);

    ctx_->PSSetConstantBuffers(0, 1, &cb_glass_);
    ctx_->PSSetShaderResources(0, 1, &blur_a_srv_);
    ctx_->Draw(4, 0);
}

void GlassRenderer::RenderFrame(float time_sec, const std::vector<GlassRect>& panels,
                                ID3D11RenderTargetView* backbuffer_rtv) {
    if (!ready_ || !scene_rtv_ || !backbuffer_rtv || width_ <= 0 || height_ <= 0) return;

    if (mode_ == GLASS_ACRYLIC) {
        // DWM 已把窗口底下的桌面模糊好；这里只把低透明度染色 + 高光面板叠上去，
        // 背景保持全透明，让模糊后的桌面透出来
        ctx_->RSSetState(rs_);
        UINT stride = 8, offset = 0;
        ctx_->IASetInputLayout(input_layout_);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->IASetVertexBuffers(0, 1, &quad_vb_, &stride, &offset);
        ctx_->VSSetShader(vs_, nullptr, 0);
        ctx_->PSSetShader(ps_glass_, nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cb_glass_);
        ctx_->PSSetSamplers(0, 1, &sampler_);
        ctx_->OMSetBlendState(blend_premul_, nullptr, 0xffffffff);
        D3D11_VIEWPORT vp{0.f, 0.f, (float)width_, (float)height_, 0.f, 1.f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->PSSetShaderResources(0, 1, &blur_a_srv_);
        for (const GlassRect& p : panels)
            DrawGlassPanel(p);
        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx_->PSSetShaderResources(0, 1, &null_srv);
        return;
    }

    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);   // 不透明阶段
    ctx_->RSSetState(rs_);

    // GLASS_CAPTURE：背景 = 抓屏纹理（CaptureBackdrop 节流更新）→ scene
    if (mode_ == GLASS_CAPTURE && cap_srv_) {
        float tx = 1.f / (float)(cap_w_ ? cap_w_ : width_);
        float ty = 1.f / (float)(cap_h_ ? cap_h_ : height_);
        DrawPass(ps_bgra_, scene_rtv_, cap_srv_, (float)width_, (float)height_, time_sec, 0, 0, tx, ty);
    } else {
        // 1) 背景 → sceneRT
        DrawPass(ps_gradient_, scene_rtv_, nullptr, (float)width_, (float)height_, time_sec, 0, 0, 0, 0);
    }
    UINT stride = 8, offset = 0;
    ctx_->IASetInputLayout(input_layout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->IASetVertexBuffers(0, 1, &quad_vb_, &stride, &offset);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->PSSetConstantBuffers(0, 1, &cb_common_);

    int bw = width_ / 4, bh = height_ / 4;
    float btx = 1.f / (float)bw, bty = 1.f / (float)bh;

    // 2) 下采样 + 两轮可分离高斯 @1/4（最终结果在 blur_a）
    DrawPass(ps_blur_, blur_a_rtv_, scene_srv_, (float)bw, (float)bh, time_sec, 0, 0, btx, bty);
    DrawPass(ps_blur_, blur_b_rtv_, blur_a_srv_, (float)bw, (float)bh, time_sec, 1, 0, btx, bty);
    DrawPass(ps_blur_, blur_a_rtv_, blur_b_srv_, (float)bw, (float)bh, time_sec, 0, 1, btx, bty);
    if (mode_ == GLASS_CAPTURE) {
        DrawPass(ps_blur_, blur_b_rtv_, blur_a_srv_, (float)bw, (float)bh, time_sec, 1, 0, btx, bty);
        DrawPass(ps_blur_, blur_a_rtv_, blur_b_srv_, (float)bw, (float)bh, time_sec, 0, 1, btx, bty);
        DrawPass(ps_blur_, blur_b_rtv_, blur_a_srv_, (float)bw, (float)bh, time_sec, 1, 0, btx, bty);
        DrawPass(ps_blur_, blur_a_rtv_, blur_b_srv_, (float)bw, (float)bh, time_sec, 0, 1, btx, bty);
    }

    // 3) 合成到后备缓冲：背景 + 玻璃面板
    DrawPass(ps_copy_, backbuffer_rtv, scene_srv_, (float)width_, (float)height_, time_sec, 0, 0, 1.f / width_, 1.f / height_);

    ctx_->OMSetBlendState(blend_premul_, nullptr, 0xffffffff);
    ctx_->PSSetShader(ps_glass_, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb_glass_);
    for (const GlassRect& p : panels)
        DrawGlassPanel(p);

    // 解绑 SRV（避免后续把 blur_a 同时当 RTV/SRV 用）
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &null_srv);
}

void GlassRenderer::Shutdown() {
    Release(quad_vb_); Release(input_layout_);
    Release(vs_); Release(ps_gradient_); Release(ps_blur_); Release(ps_copy_); Release(ps_glass_); Release(ps_bgra_);
    Release(cb_common_); Release(cb_glass_);
    Release(blend_premul_); Release(rs_); Release(sampler_);
    Release(scene_rtv_); Release(scene_srv_); Release(scene_tex_);
    Release(blur_a_rtv_); Release(blur_a_srv_); Release(blur_a_tex_);
    Release(blur_b_rtv_); Release(blur_b_srv_); Release(blur_b_tex_);
    Release(cap_srv_); Release(cap_tex_); Release(cap_staging_);
    if (cap_bitmap_) { DeleteObject(cap_bitmap_); cap_bitmap_ = nullptr; cap_bits_ = nullptr; }
    dev_ = nullptr;
    ctx_ = nullptr;
}
