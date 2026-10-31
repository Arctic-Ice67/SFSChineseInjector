// UI 主题与常用控件（风格对齐 WorldCreater：深色玻璃、圆角、克制的强调色）
#pragma once

#include <string>

#include "imgui.h"

namespace ui {

// 调色板
constexpr ImU32 kAccent = IM_COL32(94, 176, 255, 255);       // 主色（浅蓝）
constexpr ImU32 kAccentDim = IM_COL32(94, 176, 255, 70);
constexpr ImU32 kOk = IM_COL32(96, 214, 154, 255);
constexpr ImU32 kWarn = IM_COL32(255, 197, 92, 255);
constexpr ImU32 kDanger = IM_COL32(255, 112, 112, 255);
constexpr ImU32 kText = IM_COL32(232, 238, 248, 255);
constexpr ImU32 kTextDim = IM_COL32(150, 162, 180, 255);
constexpr ImU32 kCard = IM_COL32(255, 255, 255, 14);
constexpr ImU32 kCardEdge = IM_COL32(255, 255, 255, 30);

void ApplyStyle();
// 加载中文字体（优先内置 TTF，退系统字体）；返回是否用上了内置字体
bool InitFonts(const std::string& bundled_ttf);
// 绑定主窗口句柄（标题栏的最小化/关闭/拖动要用）
void SetWindow(void* hwnd);
void TitleBar(const char* title, const char* right_text);

ImFont* FontBody();
ImFont* FontSmall();
ImFont* FontBig();

// 控件
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool GhostButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool TabButton(const char* label, bool selected, const ImVec2& size = ImVec2(0, 0));
// 开关（药丸滑块）。返回 true 表示这次被点了
bool Toggle(const char* id, bool* value);
void Tag(const char* text, ImU32 color);
void SectionTitle(const char* text);
void DimText(const char* text);
void KV(const char* key, const std::string& value);
void Divider(float pad = 6.f);
void CardBegin(const char* id, const ImVec2& size, float rounding = 16.f);
void CardEnd();
// 可并排的面板（自己画底板 + 内边距），用于左右两栏
void PanelBegin(const char* id, const ImVec2& size, float rounding = 14.f);
void PanelEnd();
void ProgressBar(float frac, const ImVec2& size, ImU32 color);

// 背景：亚克力可用时只压一层暗色，否则画渐变（截图/无 DWM 时也好看）
void Background(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, bool acrylic_ok);

}  // namespace ui
