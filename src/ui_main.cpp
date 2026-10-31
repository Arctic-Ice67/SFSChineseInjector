// SFS 汉化注入器 —— 极简面板：本体汉化 / DLL 汉化 两个开关 + 开始游戏
//
// 背景：GlassRenderer（GLASS_CAPTURE）= 自抓屏 + 高斯模糊 + 玻璃卡片，
//       不依赖 DWM 版本差异；抓屏失败时退回自绘渐变。
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "app.h"
#include "catalog.h"
#include "inject_flow.h"
#include "clr_image.h"
#include "fileio.h"
#include "fontcheck.h"
#include "game.h"
#include "ini.h"
#include "json.h"
#include "patch.h"
#include "text.h"
#include "ui_theme.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr int kWinW = 380;
constexpr int kWinH = 342;
constexpr float kCardW = 348.f;
constexpr float kCardH = 282.f;

// ---------------- D3D ----------------
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_acrylic_ok = false;

// DWM 亚克力（ACCENT_ENABLE_ACRYLICBLURBEHIND）。
// 关键：clear 必须用 alpha=0，且不要在上面糊不透明底色，否则模糊就被盖掉了。
struct ACCENT_POLICY {
    int AccentState;
    int AccentFlags;
    unsigned int GradientColor;
    int AnimationId;
};
struct WINDOWCOMPOSITIONATTRIBDATA {
    int Attrib;
    void* pvData;
    size_t cbData;
};
typedef BOOL(WINAPI* PFN_SetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

bool EnableAcrylic(HWND hwnd, unsigned int tint_abgr) {
    auto fn = (PFN_SetWindowCompositionAttribute)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                               "SetWindowCompositionAttribute");
    if (!fn) return false;
    ACCENT_POLICY policy{4, 0, tint_abgr, 0};
    WINDOWCOMPOSITIONATTRIBDATA data{19, &policy, sizeof(policy)};
    return fn(hwnd, &data) != FALSE;
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}
bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL lvl;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx);
    }
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_ctx) {
        g_ctx->Release();
        g_ctx = nullptr;
    }
    if (g_dev) {
        g_dev->Release();
        g_dev = nullptr;
    }
}

// ---------------- 状态 ----------------
struct ModInfo {
    std::string path;
    std::string name;
    bool has_catalog = false;
    bool patched = false;
};

struct AppState {
    char game_path[1024] = {0};
    game::Layout layout;
    bool layout_ok = false;
    bool editing_path = false;

    bool base_on = false;
    bool dll_on = true;   // 纯注入模式：默认开启，且不再由"已安装状态"回写

    game::State state;
    size_t base_entries = 0;

    std::vector<ModInfo> mods;
    size_t mods_with_catalog = 0;
    size_t mods_patched = 0;

    game::LaunchInfo launch;
    bool game_running = false;

    std::vector<std::string> logs;
    std::vector<bool> log_error;
    std::string toast;
    bool toast_err = false;
    float toast_time = 0.f;
    std::string font_note;      // 字体操作的结果（显示在字体那一行，不弹窗）
    bool font_note_err = false;
};

AppState S;

std::mutex g_log_mu;
bool g_file_log = false;

void LogToFile(const std::string& line) {
    if (!g_file_log) return;
    static FILE* f = nullptr;
    if (!f) {
        const std::string path = fileio::join_path(app::exe_dir(), "SFSChineseInjector.log");
        f = _wfopen(text::utf8_to_wide(path).c_str(), L"ab");
        if (!f) return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 line.c_str());
    std::fflush(f);
}

void Log(const std::string& s, bool error = false) {
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        S.logs.push_back(s);
        S.log_error.push_back(error);
        if (S.logs.size() > 200) {
            S.logs.erase(S.logs.begin());
            S.log_error.erase(S.log_error.begin());
        }
    }
    LogToFile((error ? "[错误] " : "") + s);
}

// 提示一律只写日志（界面上不弹气泡）
void Toast(const std::string& s, bool error = false) { Log(s, error); }

struct JobState {
    std::thread th;
    std::atomic<bool> running{false};
    bool launch_after = false;
    std::string msg;
    bool err = false;
    bool started = false;
} g_job;

// ---------------- 数据 ----------------
std::string file_text(const std::string& path) {
    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(path, raw, err)) return std::string();
    // 去 UTF-8 BOM：记事本另存的 txt 都带 BOM，留着会让路径/文件名匹配失败
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        raw.erase(raw.begin(), raw.begin() + 3);
    }
    return std::string(raw.begin(), raw.end());
}

std::string CatalogPathFor(const std::string& dll_path) {
    return fileio::join_path(app::catalog_dir(), text::lower_ascii(fileio::file_stem(dll_path)) + ".json");
}

// 界面与游戏字体：data\fonts\font.txt 里写文件名即可切换（不写就用内置的 HarmonyOS Sans）
std::string ConfiguredFontPath() {
    const std::string dir = app::fonts_dir();
    const std::string cfg = fileio::join_path(dir, "font.txt");
    const std::string want = text::trim(file_text(cfg));
    if (!want.empty()) {
        const std::string p = fileio::join_path(dir, want);
        if (fileio::exists(p)) return p;
        Log("font.txt 指定的字体不存在：" + p + "，改用默认字体", true);
    }
    return fileio::join_path(dir, "HarmonyOS_Sans_SC_Regular.ttf");
}

std::string FontDisplayName() {
    const std::string p = ConfiguredFontPath();
    const std::string stem = fileio::file_stem(p);
    if (stem == "HarmonyOS_Sans_SC_Regular") return "HarmonyOS Sans SC";
    return stem;
}

// 汉化包实际用到的字符集合（导入字体时拿来体检）
std::u16string g_needed_chars;

std::string BuildDictText();  // 定义在下面

void BuildNeededChars() {
    const std::string text = file_text(app::data_file("translations\\zh-CN.txt"));
    const std::u16string u = text::utf8_to_utf16(text);
    std::set<char16_t> uniq;
    for (char16_t c : u) {
        if (c >= 0x20) uniq.insert(c);
    }
    g_needed_chars.clear();
    g_needed_chars.reserve(uniq.size());
    for (char16_t c : uniq) g_needed_chars.push_back(c);
}

void ReloadUiFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ui::InitFonts(ConfiguredFontPath());
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}

bool PickFontFile(std::string& out) {
    wchar_t buf[1024] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"字体文件 (*.ttf;*.otf;*.ttc)\0*.ttf;*.otf;*.ttc\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = text::wide_to_utf8(buf);
    return true;
}

void SetCustomFontNote(const std::string& s, bool err) {
    S.font_note = s;
    S.font_note_err = err;
    Log(s, err);
}

// 把用户选的字体复制进 data\fonts，做缺字体检，写 font.txt，并立刻生效
void ImportFont() {
    std::string src;
    if (!PickFontFile(src)) return;
    if (!fontcheck::looks_like_font(src)) {
        SetCustomFontNote("这个文件不是字体（只支持 ttf / otf / ttc）", true);
        return;
    }
    fontcheck::Result r;
    std::string err;
    if (!fontcheck::check(src, g_needed_chars, r, err)) {
        SetCustomFontNote("字体解析失败：" + err, true);
        return;
    }
    std::string sample;
    for (size_t i = 0; i < r.missing.size() && i < 12; ++i) {
        sample += text::utf16_to_utf8(std::u16string(1, static_cast<char16_t>(r.missing[i])));
    }
    if (r.missing.size() > 30) {
        SetCustomFontNote("缺字太多（" + std::to_string(r.missing.size()) + " 个，例如 " + sample +
                              "），换个字体吧",
                          true);
        return;
    }

    const std::string base = fileio::file_name(src);
    const std::string dst = fileio::join_path(app::fonts_dir(), "自定义_" + base);
    if (!fileio::copy_file(src, dst, true, err)) {
        SetCustomFontNote("复制字体失败：" + err, true);
        return;
    }
    const std::string cfg = fileio::join_path(app::fonts_dir(), "font.txt");
    if (!fileio::write_all(cfg, "自定义_" + base, err)) {
        SetCustomFontNote("写 font.txt 失败：" + err, true);
        return;
    }
    ReloadUiFont();

    // 运行时 mod 已装的话，把新字体也铺到游戏里（游戏在跑就等下次启动）
    std::string extra;
    if (S.layout_ok && S.state.font_runtime) {
        if (game::is_running()) {
            extra = " · 重启游戏后生效";
        } else {
            const std::string dll = fileio::join_path(app::runtime_dir(), "SFSHanRuntime.dll");
            std::string ierr;
            if (game::install_font_runtime(S.layout, dll, dst, BuildDictText(), S.state, ierr)) {
                extra = " · 已同步到游戏";
            } else {
                extra = " · 同步游戏失败（" + ierr + "）";
            }
        }
    }
    if (r.missing.empty()) {
        SetCustomFontNote("已应用：" + base + "（全覆盖）" + extra, false);
    } else {
        SetCustomFontNote("已应用：" + base + "（缺 " + std::to_string(r.missing.size()) + " 字：" + sample +
                              "）" + extra,
                          false);
    }
}

void ResetFontToDefault() {
    const std::string cfg = fileio::join_path(app::fonts_dir(), "font.txt");
    std::string err;
    fileio::remove_file(cfg, err);
    ReloadUiFont();
    if (S.layout_ok && S.state.font_runtime && !game::is_running()) {
        const std::string dll = fileio::join_path(app::runtime_dir(), "SFSHanRuntime.dll");
        const std::string def = fileio::join_path(app::fonts_dir(), "HarmonyOS_Sans_SC_Regular.ttf");
        std::string ierr;
        game::install_font_runtime(S.layout, dll, def, BuildDictText(), S.state, ierr);
    }
    SetCustomFontNote("已恢复默认字体（HarmonyOS Sans SC）", false);
}

std::string BuildDictText() {    json::Value dict;
    catalog::MergeStats stats;
    std::string err;
    if (!catalog::merge_dir(app::catalog_dir(), dict, stats, err)) return std::string();
    return json::write(dict, 2) + "\n";
}

void RefreshAll() {
    S.layout_ok = false;
    if (S.game_path[0] == 0) return;
    std::string err;
    if (!game::detect(S.game_path, S.layout, err)) {
        Log("目录不可用：" + err, true);
        return;
    }
    S.layout_ok = true;
    std::snprintf(S.game_path, sizeof(S.game_path), "%s", S.layout.root.c_str());
    // 单文件版：不写 last_game.txt，路径只保存在内存里
    game::load_state(S.layout, S.state);

    const std::string ini_text = inject_flow::TranslationText();
    if (!ini_text.empty()) {
        ini::Doc zh;
        std::string perr;
        if (ini::parse(ini_text, zh, perr)) S.base_entries = zh.entries.size();
    }

    S.mods.clear();
    S.mods_with_catalog = 0;
    S.mods_patched = 0;
    std::vector<game::ModDll> dlls;
    std::string merr;
    if (game::scan_mods(S.layout, dlls, merr)) {
        for (const auto& d : dlls) {
            if (text::lower_ascii(d.name) == "sfshanruntime.dll") continue;
            ModInfo m;
            m.path = d.path;
            m.name = d.name;
            m.has_catalog = fileio::exists(CatalogPathFor(d.path));
            m.patched = d.patched;
            if (m.has_catalog) ++S.mods_with_catalog;
            if (m.patched) ++S.mods_patched;
            S.mods.push_back(std::move(m));
        }
    }

    S.base_on = S.state.installed;
    // 注入模式不落盘，没有"已安装"可依据 —— 开关状态只由用户决定，这里不回写
}

// 初始目录优先级：
//   ① 工具自己被放进了游戏目录（手动安装最常见）→ 用那个目录，最不容易错
//   ② 上次用过的目录
//   ③ Steam / 常见路径自动探测
//   ④ 都没有 → 留空，界面提示「点这里选游戏目录」
std::string ResolveInitialGamePath() {
    char env_game[1024] = {0};
    if (GetEnvironmentVariableA("SFSHAN_GAME", env_game, sizeof(env_game)) > 0) return env_game;

    game::Layout probe;
    std::string err;
    if (game::detect(app::exe_dir(), probe, err)) return probe.root;


    const std::string guess = game::guess_default();
    if (!guess.empty() && game::detect(guess, probe, err)) return probe.root;
    return std::string();
}

// ---------------- 动作 ----------------
bool InstallBaseQuiet() {
    if (!S.layout_ok) return false;
    const std::string raw = inject_flow::TranslationText();
    if (raw.empty()) return false;
    size_t dropped = 0;
    const std::string ini = game::FilterTranslation(raw, dropped, S.layout.root);   // 去掉游戏里没有的词条，避免启动弹窗
    if (dropped) Log("已剔除 " + std::to_string(dropped) + " 条本游戏不存在的词条");
    std::string err;
    return game::install_translation(S.layout, "简体中文（汉化）", ini, true, S.state, err);
}

bool CloseDllSide() {
    if (!S.layout_ok) return true;
    std::string err;
    bool ok = true;
    for (auto& m : S.mods) {
        if (!m.patched) continue;
        std::string rerr;
        if (!patch::restore(m.path, rerr)) {
            Log("还原失败 " + m.name + "：" + rerr, true);
            ok = false;
        }
    }
    if (!game::uninstall_runtime(S.layout, S.state, err)) {
        Log("移除运行时失败：" + err, true);
        ok = false;
    }
    return ok;
}

// 纯注入模式：不往 Mods 里装任何东西，也不改 mod 的 dll（那属于静态补丁路线）。
// 真正的动作在启动游戏后由 inject_flow::Run 完成。
bool OpenDllSide() { return true; }
void SyncFromSwitches() {
    if (!S.layout_ok) return;
    if (game::is_running()) return;
    if (S.base_on) {
        if (!InstallBaseQuiet()) Log("本体汉化安装失败", true);
    } else if (S.state.installed) {
        std::string err;
        if (!game::uninstall_translation(S.layout, S.state, err)) Log("卸载本体失败：" + err, true);
    }
    if (S.dll_on) {
        OpenDllSide();
    } else if (S.state.font_runtime || S.mods_patched > 0) {
        CloseDllSide();
    }
}

void StartApplyJob(bool launch_after) {
    if (g_job.running) return;
    if (g_job.th.joinable()) g_job.th.join();
    if (!S.layout_ok) {
        Toast("先选游戏目录", true);
        return;
    }
    if (game::is_running()) {
        Toast("游戏正在运行，请先退出", true);
        return;
    }
    g_job.launch_after = launch_after;
    g_job.msg.clear();
    g_job.err = false;
    g_job.running = true;
    g_job.started = true;
    g_job.th = std::thread([]() {
        try {
            SyncFromSwitches();
            if (S.base_on && !S.state.installed) {
                g_job.msg = "本体汉化没成功，看日志";
                g_job.err = true;
            } else {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s%s", S.base_on ? "本体已开" : "本体已关",
                              S.dll_on ? " · DLL 已开" : " · DLL 已关");
                g_job.msg = buf;
            }
        } catch (const std::exception& e) {
            g_job.msg = std::string("出错：") + e.what();
            g_job.err = true;
        } catch (...) {
            g_job.msg = "出错（未知异常）";
            g_job.err = true;
        }
        g_job.running = false;
    });
}

void DoLaunch() {
    std::string err;
    game::LaunchInfo info;
    if (!game::launch(S.layout, info, err)) {
        Toast("启动失败：" + err, true);
        return;
    }
    S.launch = info;
    S.game_running = true;

    // DLL 汉化：启动后等进程起来，纯内存注入（不写任何文件）
    if (S.dll_on) {
        const game::Layout layout = S.layout;
        std::thread([layout]() {
            std::string ierr;
            if (!inject_flow::Run(layout, 60000, ierr)) Log("注入失败：" + ierr, true);
            else Log("已注入（内存运行，磁盘无残留）");
        }).detach();
    }
}

void LaunchGame() {
    if (!S.layout_ok) {
        Toast("先选游戏目录", true);
        return;
    }
    if (game::is_running()) {
        Toast("游戏已经在运行了", true);
        return;
    }
    StartApplyJob(true);
}

void PollJob() {
    if (!g_job.started || g_job.running) return;
    if (g_job.th.joinable()) g_job.th.join();
    g_job.started = false;
    RefreshAll();
    if (!g_job.msg.empty()) Toast(g_job.msg, g_job.err);
    if (g_job.launch_after) {
        g_job.launch_after = false;
        DoLaunch();
    }
}

void PollGame() {
    if (!S.launch.process) return;
    if (game::process_alive(S.launch)) return;
    game::close_launch(S.launch);
    S.game_running = false;
    Log("游戏已退出");
}

// ---------------- 界面 ----------------
std::string ShortenPath(const std::string& p, size_t max_chars) {
    if (p.size() <= max_chars) return p;
    return p.substr(0, max_chars / 3) + "…" + p.substr(p.size() - (max_chars - max_chars / 3 - 1));
}

// 一行「标签 + 开关」，整行水平居中（start_x/content_w 用卡片内容区，避免重复加 padding）
bool CenteredToggleRow(const char* id, const char* label, bool* value, float start_x, float content_w,
                       bool disabled) {
    const float toggle_w = 48.f;
    const float lw = ImGui::CalcTextSize(label).x;
    const float gap = 22.f;
    const float total = lw + gap + toggle_w;
    ImGui::SetCursorPosX(start_x + (content_w - total) * 0.5f);
    ImGui::AlignTextToFramePadding();
    const float y0 = ImGui::GetCursorPosY();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.f, gap);
    ImGui::SetCursorPosY(y0 - 2.f);
    if (disabled) ImGui::BeginDisabled();
    bool v = *value;
    const bool changed = ui::Toggle(id, &v);
    if (changed) *value = v;
    if (disabled) ImGui::EndDisabled();
    return changed;
}

void DrawCard() {
    const bool disabled = S.game_running || g_job.running;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    ui::CardBegin("##card", ImVec2(kCardW, kCardH), 24.f);

    // 内容区实际可用宽度 / 起始 x（CardBegin 已经带 padding，这里不要再加一遍）
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float start_x = ImGui::GetCursorPosX();

    // 游戏目录（小字、居中、可点开改）
    if (!S.editing_path) {
        const std::string shown = S.layout_ok ? ShortenPath(S.layout.root, 42) : "点这里选游戏目录";
        const float tw = ImGui::CalcTextSize(shown.c_str()).x + 16.f;
        ImGui::SetCursorPosX(start_x + std::max(0.f, (content_w - tw) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 24));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 34));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.74f, 0.80f, 0.90f, 1.f));
        ImGui::PushFont(ui::FontSmall());
        if (ImGui::Button(shown.c_str(), ImVec2(tw, 20.f))) S.editing_path = true;
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
    } else {
        ImGui::SetCursorPosX(start_x);
        ImGui::SetNextItemWidth(content_w - 122.f);
        if (ImGui::InputTextWithHint("##p", "游戏目录", S.game_path, sizeof(S.game_path),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            RefreshAll();
            if (S.layout_ok) S.editing_path = false;
        }
        ImGui::SameLine(0, 6);
        if (ui::GhostButton("探测", ImVec2(52, 0))) {
            const std::string p = game::guess_default();
            if (p.empty()) {
                Toast("没找到，请手动填", true);
            } else {
                std::snprintf(S.game_path, sizeof(S.game_path), "%s", p.c_str());
                RefreshAll();
                if (S.layout_ok) S.editing_path = false;
            }
        }
        ImGui::SameLine(0, 6);
        if (ui::GhostButton("确定", ImVec2(52, 0))) {
            RefreshAll();
            if (S.layout_ok) S.editing_path = false;
        }
    }

    ImGui::Dummy(ImVec2(0, 10));

    // 字体行：显示当前字体 + 更换 / 恢复默认
    {
        const std::string name = FontDisplayName();
        const std::string label = "字体：" + name;
        const std::string cfg_cur = text::trim(file_text(fileio::join_path(app::fonts_dir(), "font.txt")));
        const bool custom = !cfg_cur.empty();
        ImGui::PushFont(ui::FontSmall());
        const float lw = ImGui::CalcTextSize(label.c_str()).x;
        const float bw = 44.f;
        const float total = lw + 12.f + bw + (custom ? (12.f + bw) : 0.f);
        ImGui::SetCursorPosX(start_x + std::max(0.f, (content_w - total) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.74f, 0.80f, 0.90f, 1.f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 12);
        if (ImGui::Button("更换", ImVec2(bw, 20.f))) ImportFont();
        if (custom) {
            ImGui::SameLine(0, 12);
            if (ImGui::Button("默认", ImVec2(bw, 20.f))) ResetFontToDefault();
        }
        ImGui::PopFont();
        if (!S.font_note.empty()) {
            ImGui::PushFont(ui::FontSmall());
            ImGui::PushStyleColor(ImGuiCol_Text, S.font_note_err ? ImVec4(1.f, 0.52f, 0.52f, 1.f)
                                                                : ImVec4(0.60f, 0.72f, 0.82f, 1.f));
            const float nw = ImGui::CalcTextSize(S.font_note.c_str()).x;
            ImGui::SetCursorPosX(start_x + std::max(0.f, (content_w - nw) * 0.5f));
            ImGui::TextWrapped("%s", S.font_note.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }

    ImGui::Dummy(ImVec2(0, 12));
    if (CenteredToggleRow("##t_base", "本体汉化", &S.base_on, start_x, content_w, disabled)) {
        StartApplyJob(false);
    }
    ImGui::Dummy(ImVec2(0, 12));
    if (CenteredToggleRow("##t_dll", "DLL 汉化", &S.dll_on, start_x, content_w, disabled)) {
        StartApplyJob(false);
    }

    ImGui::Dummy(ImVec2(0, 18));

    // 开始游戏（居中）
    const float btn_w = 236.f;
    ImGui::SetCursorPosX(start_x + (content_w - btn_w) * 0.5f);
    if (g_job.running) {
        static int dots = 0;
        static float t = 0.f;
        t += ImGui::GetIO().DeltaTime;
        if (t > 0.35f) {
            t = 0.f;
            dots = (dots + 1) % 4;
        }
        const std::string label = "正在应用" + std::string(dots, '.');
        ui::GhostButton(label.c_str(), ImVec2(btn_w, 48.f));
    } else if (S.game_running) {
        if (ui::GhostButton("游戏运行中…", ImVec2(btn_w, 48.f))) Toast("游戏已经在跑了");
    } else {
        if (ui::PrimaryButton(S.layout_ok ? "开始游戏" : "先选游戏目录", ImVec2(btn_w, 48.f))) LaunchGame();
    }

    ui::CardEnd();
    ImGui::PopStyleVar();
}

void BuildUI(float dt) {
    PollJob();
    PollGame();

    static int frame = 0;
    static bool verbose = false;
    static bool verbose_init = false;
    if (!verbose_init) {
        verbose_init = true;
        char b[8] = {0};
        verbose = GetEnvironmentVariableA("SFSHAN_VERBOSE", b, sizeof(b)) > 0;
    }
    ++frame;
    if (verbose && frame % 60 == 0) LogToFile("心跳 frame=" + std::to_string(frame));

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav);

    // 背景先铺（亚克力负责模糊，这里只压一层很薄的暗色，太重会把模糊盖掉）
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = vp->WorkPos;
        const ImVec2 p1(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
        if (g_acrylic_ok) {
            dl->AddRectFilled(p0, p1, IM_COL32(12, 16, 22, 40));
        } else {
            ui::Background(dl, p0, p1, false);
        }
    }

    ui::TitleBar("SFS 中文注入器", nullptr);

    const float x = std::max(10.f, (vp->WorkSize.x - kCardW) * 0.5f);
    const float y = std::max(40.f, 32.f + (vp->WorkSize.y - 32.f - kCardH) * 0.5f);
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("##cardhost", ImVec2(kCardW, kCardH), ImGuiChildFlags_None);
    DrawCard();
    ImGui::EndChild();

    // 不再弹气泡提示：所有结果只写日志（exe 同级的 SFSChineseInjector.log）

    ImGui::End();
}

// ---------------- 窗口过程 ----------------
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (g_dev && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_swap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        char vb[8] = {0};
        // 正式版不写任何日志；排查时设 SFSHAN_VERBOSE=1 才会落 SFSHanInjector.log
        g_file_log = ::GetEnvironmentVariableA("SFSHAN_VERBOSE", vb, sizeof(vb)) > 0 && vb[0] == '1';
    }
    Log("=== 启动 v" + std::string(app::kVersion) + " ===");

    WNDCLASSEXW wc{sizeof(wc), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, nullptr, nullptr, nullptr, nullptr,
                   nullptr, L"SFSChineseInjectorUI", nullptr};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SFS 中文注入器",
                             WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, kWinW, kWinH, nullptr, nullptr, wc.hInstance,
                             nullptr);
    if (!g_hwnd || !CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        return 1;
    }
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    {
        INT pref = 2;  // DWMWA_WINDOW_CORNER_PREFERENCE = ROUND
        DwmSetWindowAttribute(g_hwnd, 33, &pref, sizeof(pref));
    }
    {
        RECT r;
        GetWindowRect(g_hwnd, &r);
        SetWindowPos(g_hwnd, nullptr, (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                     (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
    }
    // DWM 亚克力：真正做模糊的是系统，我们只负责把窗口画成半透明
    g_acrylic_ok = EnableAcrylic(g_hwnd, 0x6E18120E);
    {
        char buf[8] = {0};
        if (GetEnvironmentVariableA("SFSHAN_NO_ACRYLIC", buf, sizeof(buf)) > 0 && buf[0] == '1') {
            g_acrylic_ok = false;
        }
    }
    Log(g_acrylic_ok ? "亚克力背景：已启用" : "亚克力不可用，退回自绘渐变");
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    {
        // 注意：不要用 DwmExtendFrameIntoClientArea，会把亚克力糊掉（背景变纯黑）
        INT pref = 2;  // DWMWA_WINDOW_CORNER_PREFERENCE = ROUND
        DwmSetWindowAttribute(g_hwnd, 33, &pref, sizeof(pref));
        BOOL dark = TRUE;  // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
        COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(g_hwnd, 34, &none, sizeof(none));  // DWMWA_BORDER_COLOR
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ui::ApplyStyle();
    ui::InitFonts(ConfiguredFontPath());
    ui::SetWindow(g_hwnd);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // 初始目录：工具所在目录（若就是游戏目录）→ 上次记录 → 自动探测
    {
        const std::string initial = ResolveInitialGamePath();
        if (!initial.empty()) std::snprintf(S.game_path, sizeof(S.game_path), "%s", initial.c_str());
    }
    RefreshAll();
    Log("扫描完成");
    if (S.layout_ok) {
        Log("游戏目录：" + S.layout.root);
    } else {
        Log("请先指定游戏目录", true);
    }

    bool running = true;
    int frame_no = 0;
    std::atomic<unsigned long long> frame_ticks{0};
    // 看门狗：每秒看一次渲染循环有没有推进，卡住时会写进日志（方便定位是不是 Present/DWM 的问题）
    std::atomic<bool> watchdog_stop{false};
    std::thread watchdog([&frame_ticks, &watchdog_stop]() {
        unsigned long long last = 0;
        while (!watchdog_stop.load()) {
            for (int i = 0; i < 20 && !watchdog_stop.load(); ++i) Sleep(100);
            if (watchdog_stop.load()) break;
            const unsigned long long now = frame_ticks.load();
            if (now == last) {
                LogToFile("看门狗：渲染循环停滞（帧数 " + std::to_string(now) + "）");
            }
            last = now;
        }
    });
    bool autotest = false;
    bool autotest_init = false;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (!autotest_init) {
            autotest_init = true;
            char b[8] = {0};
            autotest = GetEnvironmentVariableA("SFSHAN_AUTOTEST", b, sizeof(b)) > 0 && b[0] == '1';
        }
        ++frame_no;
        frame_ticks.fetch_add(1);
        if (autotest) {
            if (frame_no == 60 && !S.base_on) {
                S.base_on = true;
                S.dll_on = true;
                StartApplyJob(false);
            } else if (frame_no == 120) {
                LaunchGame();       // 启动 + 注入
            } else if (frame_no == 900) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        BuildUI(ImGui::GetIO().DeltaTime);
        ImGui::Render();

        if (g_acrylic_ok) {
            // 交给 DWM 合成：clear 用 alpha=0，内容按半透明画，模糊的桌面就从底下透出来
            const float clear[4] = {0.f, 0.f, 0.f, 0.f};
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->ClearRenderTargetView(g_rtv, clear);
        } else {
            const float clear[4] = {0.f, 0.f, 0.f, 1.f};
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->ClearRenderTargetView(g_rtv, clear);
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // 不能用 Present(1,0)：透过后备缓冲(alpha=0)+亚克力时，DWM 可能长时间不返回，
        // 整个渲染循环就卡在 Present 上（表现为窗口「未响应」）。改成不等待 + 自己限帧。
        g_swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);

        // 限帧 ~60
        {
            static LARGE_INTEGER freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return f;
            }();
            static LARGE_INTEGER last{};
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (last.QuadPart && freq.QuadPart) {
                const double ms = 1000.0 * double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
                if (ms < 16.0) {
                    const DWORD sleep_ms = static_cast<DWORD>(16.0 - ms);
                    if (sleep_ms) Sleep(sleep_ms);
                    QueryPerformanceCounter(&now);
                }
            }
            last = now;
        }
    }

    watchdog_stop = true;
    if (watchdog.joinable()) watchdog.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    return 0;
}
