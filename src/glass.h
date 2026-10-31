// 液态玻璃渲染器（DX11 移植自 imgui-liquid-glass-main 的 GLSL 实现）
//
// 管线：动画渐变背景 → sceneRT；1/4 分辨率可分离高斯模糊 → blurRT；
// 玻璃面板逐个以像素着色器合成（SDF 圆角矩形 + 边缘折射 + 七采样色散 +
// rim/高光/内外阴影），先画进后备缓冲，ImGui 再在其上绘制面板内容。
// 数学与参数来自参考实现的 liquid_glass.frag / glass_math / glass_lighting
// （Adapted from AndroidLiquidGlassView, MIT）。

#pragma once

#include <d3d11.h>
#include <vector>

struct ImVec2;
struct ImVec4;

struct GlassRect {
    float x0, y0, x1, y1;      // 屏幕像素坐标
    float rounding = 22.f;
    float alpha = 1.f;         // 整体不透明度
};

// 背景来源模式
enum GlassMode {
    GLASS_GRADIENT = 0,   // 自绘动画渐变（无桌面、无外部依赖）
    GLASS_ACRYLIC  = 1,   // DWM 亚克力：真实桌面模糊，模糊强度系统定
    GLASS_CAPTURE  = 2,   // 自采桌面 + 自跑高斯模糊：强度可调，最接近参考效果
};

class GlassRenderer {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);
    void Resize(int width, int height);
    void SetMode(int mode);
    int Mode() const { return mode_; }
    // 调试：只抓第一帧（背景冻结为启动时的桌面，便于无 WDA 截屏诊断）
    void SetSingleShot(bool v) { single_shot_ = v; }
    // 实时抓屏：拖动/缩放期间 true；静止时 false（保持最后画面，窗口可被正常截屏）
    void SetLiveCapture(bool v) { live_capture_ = v; }
    // GLASS_CAPTURE 模式：抓取窗口正下方的屏幕区域（需配合
    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 防止抓到自己）
    void CaptureBackdrop(HWND hwnd);
    // 在 ImGui::RenderDrawData 之前调用：绘制背景 + 模糊 + 玻璃面板到后备缓冲
    void RenderFrame(float time_sec, const std::vector<GlassRect>& panels,
                     ID3D11RenderTargetView* backbuffer_rtv);
    void Shutdown();

private:
    bool CreateRenderTargets(int width, int height);
    void DrawPass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv,
                  ID3D11ShaderResourceView* srv, float vp_w, float vp_h,
                  float time, float dirx, float diry, float texelx, float texely);
    void DrawGlassPanel(const GlassRect& panel);

    bool ready_ = false;                       // Init 完整成功后才允许渲染
    bool single_shot_ = false;                 // 调试：仅首帧抓屏
    bool live_capture_ = false;                // 拖动/缩放期间实时抓屏
    bool cap_done_ = false;
    int mode_ = GLASS_GRADIENT;
    // 桌面捕获（GLASS_CAPTURE）
    ID3D11Texture2D* cap_tex_ = nullptr;       // B8G8R8A8，GDI 抓屏直传
    ID3D11RenderTargetView* cap_rtv_pad_ = nullptr; // 未用占位，保持 16 字节对齐习惯
    ID3D11ShaderResourceView* cap_srv_ = nullptr;
    ID3D11Texture2D* cap_staging_ = nullptr;   // CPU 写入中转
    HBITMAP cap_bitmap_ = nullptr;             // GDI DIB
    void* cap_bits_ = nullptr;
    int cap_w_ = 0, cap_h_ = 0;
    unsigned long long last_cap_ms_ = 0;
    void EnsureCaptureSize(int w, int h);
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    int width_ = 0, height_ = 0;

    ID3D11Buffer* quad_vb_ = nullptr;          // 全屏三角形带
    ID3D11InputLayout* input_layout_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_gradient_ = nullptr; // 背景渐变
    ID3D11PixelShader* ps_blur_ = nullptr;     // 可分离高斯（方向由 CB 传入）
    ID3D11PixelShader* ps_copy_ = nullptr;     // 纹理复制（背景上屏）
    ID3D11PixelShader* ps_glass_ = nullptr;    // 玻璃合成
    ID3D11PixelShader* ps_bgra_ = nullptr;     // 抓屏 BGRA→RGBA
    ID3D11Buffer* cb_common_ = nullptr;        // 渐变/模糊共用 CB
    ID3D11Buffer* cb_glass_ = nullptr;         // 每面板参数 CB
    ID3D11BlendState* blend_premul_ = nullptr; // 玻璃用预乘 alpha
    ID3D11RasterizerState* rs_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;

    ID3D11Texture2D* scene_tex_ = nullptr;
    ID3D11RenderTargetView* scene_rtv_ = nullptr;
    ID3D11ShaderResourceView* scene_srv_ = nullptr;
    ID3D11Texture2D* blur_a_tex_ = nullptr;    // 1/4 分辨率，模糊结果最终在 blur_a
    ID3D11RenderTargetView* blur_a_rtv_ = nullptr;
    ID3D11ShaderResourceView* blur_a_srv_ = nullptr;
    ID3D11Texture2D* blur_b_tex_ = nullptr;
    ID3D11RenderTargetView* blur_b_rtv_ = nullptr;
    ID3D11ShaderResourceView* blur_b_srv_ = nullptr;
};
