#include "ui_theme.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "fileio.h"
#include "text.h"

namespace ui {
namespace {

ImFont* g_body = nullptr;
ImFont* g_small = nullptr;
ImFont* g_big = nullptr;
HWND g_hwnd = nullptr;
bool g_maxed = false;
RECT g_pre_max{};
float g_cap_anim[3] = {0.f, 0.f, 0.f};
// 拖动状态：屏幕坐标 + 按下时的窗口矩形
bool g_dragging = false;
POINT g_drag_cursor0{0, 0};
RECT g_drag_rect0{0, 0, 0, 0};

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

void ToggleMaximize(HWND hwnd) {
    if (!g_maxed) {
        GetWindowRect(hwnd, &g_pre_max);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        SetWindowPos(hwnd, nullptr, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left,
                     mi.rcWork.bottom - mi.rcWork.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        g_maxed = true;
    } else {
        SetWindowPos(hwnd, nullptr, g_pre_max.left, g_pre_max.top, g_pre_max.right - g_pre_max.left,
                     g_pre_max.bottom - g_pre_max.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        g_maxed = false;
    }
}

bool CaptionButton(const char* id, int kind) {
    const float w = 46.f, h = 32.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(0);

    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.1f) dt = 0.1f;
    const float target = (hovered || held) ? 1.f : 0.f;
    const int idx = kind < 0 ? 0 : (kind > 2 ? 2 : kind);
    g_cap_anim[idx] += (target - g_cap_anim[idx]) * (dt * 16.f);
    const float a = clamp01(g_cap_anim[idx]);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = 1.f + 0.12f * a;
    ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
    ImVec2 hs(w * 0.5f * scale - 3.f, h * 0.5f * scale - 2.f);

    if (kind == 2 && a > 0.01f) {
        dl->AddRectFilled(ImVec2(c.x - hs.x, c.y - hs.y), ImVec2(c.x + hs.x, c.y + hs.y),
                          IM_COL32(235, 80, 80, static_cast<int>(200.f * a)), 8.f);
    } else if (a > 0.01f) {
        dl->AddRectFilled(ImVec2(c.x - hs.x, c.y - hs.y), ImVec2(c.x + hs.x, c.y + hs.y),
                          IM_COL32(255, 255, 255, static_cast<int>(44.f * a)), 8.f);
    }
    const ImU32 col = IM_COL32(255, 255, 255, static_cast<int>(200.f + 55.f * a));
    const float th = 1.4f + 1.0f * a;
    const float g = 5.0f * (1.f + 0.10f * a);
    if (kind == 0) {
        dl->AddLine(ImVec2(c.x - g - 1.f, c.y), ImVec2(c.x + g + 1.f, c.y), col, th);
    } else if (kind == 1) {
        dl->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), col, 0.f, 0, th);
    } else {
        dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), col, th);
        dl->AddLine(ImVec2(c.x - g, c.y + g), ImVec2(c.x + g, c.y - g), col, th);
    }
    return clicked;
}

// 通用按钮：fill/border/text 由调用方决定
bool StyledButton(const char* label, ImU32 fill, ImU32 border, ImU32 text_col, const ImVec2& size,
                  bool disabled) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 text_sz = ImGui::CalcTextSize(label);
    ImVec2 sz = size;
    if (sz.x <= 0.f) sz.x = text_sz.x + st.FramePadding.x * 2.f + 14.f;
    if (sz.y <= 0.f) sz.y = 34.f;

    ImVec2 p = ImGui::GetCursorScreenPos();
    if (disabled) ImGui::BeginDisabled();
    ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(0) && !disabled;
    if (disabled) ImGui::EndDisabled();

    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.1f) dt = 0.1f;
    ImGui::PushID(label);
    static std::vector<float> anims;  // 简化：按 label 的哈希做轻量缓动
    (void)anims;
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = hovered ? (held ? 1.f : 0.75f) : 0.f;
    const int boost = static_cast<int>(28.f * t);
    auto add = [&](ImU32 c, int extra) {
        return IM_COL32(static_cast<int>((c >> IM_COL32_R_SHIFT) & 0xFF),
                        static_cast<int>((c >> IM_COL32_G_SHIFT) & 0xFF),
                        static_cast<int>((c >> IM_COL32_B_SHIFT) & 0xFF),
                        std::min(255, static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFF) + extra));
    };
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), add(fill, boost), 10.f);
    if (border != 0) dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), add(border, boost / 2), 10.f, 0, 1.f);
    const ImVec2 tp(p.x + (sz.x - text_sz.x) * 0.5f, p.y + (sz.y - text_sz.y) * 0.5f);
    dl->AddText(tp, disabled ? IM_COL32(140, 148, 160, 200) : text_col, label);
    (void)dt;
    return clicked;
}

}  // namespace

void SetWindow(void* hwnd) { g_hwnd = static_cast<HWND>(hwnd); }

void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 6);
    s.ScrollbarSize = 10.f;
    s.WindowRounding = 14.f;
    s.ChildRounding = 14.f;
    s.FrameRounding = 8.f;
    s.PopupRounding = 12.f;
    s.GrabRounding = 8.f;
    s.TabRounding = 8.f;
    s.WindowBorderSize = 0.f;
    s.ChildBorderSize = 0.f;
    s.FrameBorderSize = 0.f;
    s.Colors[ImGuiCol_Text] = ImVec4(0.91f, 0.93f, 0.97f, 1.f);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.59f, 0.63f, 0.70f, 1.f);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.09f, 0.12f, 0.98f);
    s.Colors[ImGuiCol_Border] = ImVec4(1.f, 1.f, 1.f, 0.10f);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(1.f, 1.f, 1.f, 0.07f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1.f, 1.f, 1.f, 0.12f);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(1.f, 1.f, 1.f, 0.16f);
    s.Colors[ImGuiCol_Button] = ImVec4(1.f, 1.f, 1.f, 0.08f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.f, 1.f, 1.f, 0.14f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(1.f, 1.f, 1.f, 0.20f);
    s.Colors[ImGuiCol_Header] = ImVec4(0.37f, 0.69f, 1.f, 0.25f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.37f, 0.69f, 1.f, 0.35f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(0.37f, 0.69f, 1.f, 0.45f);
    s.Colors[ImGuiCol_Separator] = ImVec4(1.f, 1.f, 1.f, 0.10f);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 1.f, 1.f, 0.20f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.f, 1.f, 1.f, 0.32f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.f, 1.f, 1.f, 0.45f);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(0.37f, 0.69f, 1.f, 1.f);
    s.Colors[ImGuiCol_SliderGrab] = ImVec4(0.37f, 0.69f, 1.f, 0.9f);
    s.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.37f, 0.69f, 1.f, 0.35f);
}

bool InitFonts(const std::string& bundled_ttf) {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* f = nullptr;
    bool used_bundled = false;
    if (!bundled_ttf.empty() && fileio::exists(bundled_ttf)) {
        f = io.Fonts->AddFontFromFileTTF(bundled_ttf.c_str(), 18.0f);
        used_bundled = f != nullptr;
    }
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f);
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttf", 18.0f);
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f);
    if (!f) f = io.Fonts->AddFontDefault();
    g_body = f;
    // 小号/大号优先复用同一份内置字体（别再把 20 MB 的 msyh.ttc 反复解析）
    const char* small_src = used_bundled ? bundled_ttf.c_str() : "C:\\Windows\\Fonts\\msyh.ttc";
    g_small = io.Fonts->AddFontFromFileTTF(small_src, 15.0f);
    if (!g_small) g_small = f;
    g_big = io.Fonts->AddFontFromFileTTF(small_src, 22.0f);
    if (!g_big) g_big = f;
    io.FontDefault = f;
    return used_bundled;
}

ImFont* FontBody() { return g_body; }
ImFont* FontSmall() { return g_small; }
ImFont* FontBig() { return g_big; }

void TitleBar(const char* title, const char* right_text) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##titlebar", ImVec2(0, 32), ImGuiChildFlags_None);
    const float w = ImGui::GetWindowWidth();
    const float text_y = (32.f - ImGui::GetTextLineHeight()) * 0.5f;

    ImGui::SetCursorPos(ImVec2(16, text_y));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.95f, 0.99f, 1.f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (right_text && *right_text) {
        ImGui::SameLine(0, 14);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.f));
        ImGui::TextUnformatted(right_text);
        ImGui::PopStyleColor();
    }

    const float bw = 46.f;
    const float bx = w - bw * 2.f;
    ImGui::SetCursorPos(ImVec2(bx, 0.f));
    if (CaptionButton("##min", 0) && g_hwnd) ShowWindow(g_hwnd, SW_MINIMIZE);
    ImGui::SetCursorPos(ImVec2(bx + bw, 0.f));
    if (CaptionButton("##close", 2) && g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##drag", ImVec2(bx - 4.f > 40.f ? bx - 4.f : 40.f, 32));
    if (ImGui::IsItemActivated() && g_hwnd) {
        // 记下按下时的「屏幕光标位置」和「窗口位置」——不能用客户区位移，
        // 否则窗口一移动，客户坐标跟着变，算出来的位移会反向，表现为一直回弹
        GetCursorPos(&g_drag_cursor0);
        GetWindowRect(g_hwnd, &g_drag_rect0);
        g_dragging = true;
    }
    if (ImGui::IsItemActive() && g_dragging && g_hwnd) {
        POINT cur;
        GetCursorPos(&cur);
        SetWindowPos(g_hwnd, nullptr, g_drag_rect0.left + (cur.x - g_drag_cursor0.x),
                     g_drag_rect0.top + (cur.y - g_drag_cursor0.y), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!ImGui::IsItemActive() && ImGui::IsMouseReleased(0)) g_dragging = false;
    if (!ImGui::IsMouseDown(0)) g_dragging = false;
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool PrimaryButton(const char* label, const ImVec2& size) {
    return StyledButton(label, IM_COL32(74, 150, 232, 210), IM_COL32(120, 180, 255, 120),
                        IM_COL32(255, 255, 255, 255), size, false);
}

bool GhostButton(const char* label, const ImVec2& size) {
    return StyledButton(label, IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 34),
                        IM_COL32(228, 234, 245, 255), size, false);
}

bool TabButton(const char* label, bool selected, const ImVec2& size) {
    ImVec2 sz = size;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    if (sz.x <= 0.f) sz.x = ts.x + 30.f;
    if (sz.y <= 0.f) sz.y = 32.f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = selected ? IM_COL32(94, 176, 255, 46)
                              : (hovered ? IM_COL32(255, 255, 255, 20) : IM_COL32(255, 255, 255, 0));
    if ((bg >> IM_COL32_A_SHIFT) & 0xFF) dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 9.f);
    if (selected) {
        const float bar_w = std::max(18.f, sz.x * 0.5f);
        dl->AddRectFilled(ImVec2(p.x + (sz.x - bar_w) * 0.5f, p.y + sz.y - 2.5f),
                          ImVec2(p.x + (sz.x + bar_w) * 0.5f, p.y + sz.y), kAccent, 2.f);
    }
    const ImU32 col = selected ? IM_COL32(240, 246, 255, 255) : IM_COL32(168, 178, 194, 255);
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) * 0.5f, p.y + (sz.y - ts.y) * 0.5f), col, label);
    return clicked;
}

bool Toggle(const char* id, bool* value) {
    const float h = 26.f;
    const float w = 48.f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool changed = false;
    if (ImGui::IsItemClicked(0)) {
        *value = !*value;
        changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();

    static std::unordered_map<unsigned int, float> store;
    const ImGuiID wid = ImGui::GetItemID();
    float& t = store[static_cast<unsigned int>(wid)];
    float target = *value ? 1.f : 0.f;
    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.1f) dt = 0.1f;
    t += (target - t) * (dt * 18.f);
    if (t < 0.002f) t = 0.f;
    if (t > 0.998f) t = 1.f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 track = ImGui::ColorConvertFloat4ToU32(
        ImVec4(0.37f + 0.02f * t + (hovered ? 0.03f : 0.f), 0.45f + 0.24f * t, 0.55f + 0.45f * t,
               0.35f + 0.55f * t));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, h * 0.5f);
    if (hovered && t < 0.5f) {
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 60), h * 0.5f, 0, 1.f);
    }
    const float knob_x = p.x + h * 0.5f + (w - h) * t;
    dl->AddCircleFilled(ImVec2(knob_x, p.y + h * 0.5f), h * 0.5f - 3.5f, IM_COL32(255, 255, 255, 245), 24);
    return changed;
}

void Tag(const char* text, ImU32 color) {    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ts.x + 16.f, ts.y + 6.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), (color & 0x00FFFFFF) | (46u << 24), 7.f);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), (color & 0x00FFFFFF) | (110u << 24), 7.f, 0, 1.f);
    dl->AddText(ImVec2(p.x + 8.f, p.y + 3.f), color, text);
    ImGui::Dummy(sz);
    ImGui::SameLine(0, 8);
}

void SectionTitle(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.78f, 0.88f, 1.f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void DimText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void KV(const char* key, const std::string& value) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.f));
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    ImGui::SameLine(132.f);
    ImGui::TextWrapped("%s", value.c_str());
}

void Divider(float pad) {
    ImGui::Dummy(ImVec2(0, pad * 0.5f));
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 26), 1.f);
    ImGui::Dummy(ImVec2(0, pad * 0.5f));
}

void CardBegin(const char* id, const ImVec2& size, float rounding) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 sz = size;
    if (sz.x <= 0.f) sz.x = avail.x;
    if (sz.y <= 0.f) sz.y = 120.f;
    // 只占位 + 内边距，不画底板（背景就是亚克力本身）
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::BeginChild(id, sz, ImGuiChildFlags_None);
    (void)rounding;
}

void CardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PanelBegin(const char* id, const ImVec2& size, float rounding) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), kCard, rounding);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), kCardEdge, rounding, 0, 1.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    ImGui::BeginChild(id, size, ImGuiChildFlags_None);
}

void PanelEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void ProgressBar(float frac, const ImVec2& size, ImU32 color) {
    frac = clamp01(frac);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    if (sz.x <= 0.f) sz.x = ImGui::GetContentRegionAvail().x;
    if (sz.y <= 0.f) sz.y = 6.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(255, 255, 255, 22), sz.y * 0.5f);
    if (frac > 0.f) {
        dl->AddRectFilled(p, ImVec2(p.x + sz.x * frac, p.y + sz.y), color, sz.y * 0.5f);
    }
    ImGui::Dummy(sz);
}

void Background(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, bool acrylic_ok) {
    if (acrylic_ok) {
        // 亚克力已经把桌面模糊好了，只压一层很薄的暗色保证文字对比
        dl->AddRectFilled(p0, p1, IM_COL32(14, 18, 24, 170));
        return;
    }
    // 兜底：自绘渐变 + 两个柔光斑
    dl->AddRectFilledMultiColor(p0, p1, IM_COL32(20, 28, 44, 255), IM_COL32(16, 20, 34, 255),
                                IM_COL32(12, 14, 24, 255), IM_COL32(22, 26, 40, 255));
    const ImVec2 c1(p1.x * 0.22f, p1.y * 0.18f);
    const ImVec2 c2(p1.x * 0.80f, p1.y * 0.10f);
    dl->AddCircleFilled(c1, p1.y * 0.45f, IM_COL32(52, 96, 160, 26), 64);
    dl->AddCircleFilled(c2, p1.y * 0.34f, IM_COL32(90, 60, 140, 20), 64);
}

}  // namespace ui
