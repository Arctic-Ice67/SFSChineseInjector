// 游戏目录 / 本体汉化安装 / 卸载回滚。
//
// 本体汉化的真相（来自反编译的 SFS.Translations）：
//   * 自定义语言 = <游戏>_Data\Custom Translations\<名字>.txt（INI 分组，缺词条自动回落英文）
//   * 当前语言记在 <游戏>\Saving\Settings\LanguageSettings_2.txt，内容是
//     {"CodeName":"<名字>","Custom":true}
//   * 字体走 TranslationManager.fonts；中文要额外注入字体（由运行时 mod 负责）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace game {

struct Layout {
    std::string root;            // 游戏根目录（有 Spaceflight Simulator.exe）
    std::string exe;             // 根目录\Spaceflight Simulator.exe
    std::string data_dir;        // 根目录\Spaceflight Simulator_Data
    std::string managed;         // data_dir\Managed
    std::string custom_trans;    // data_dir\Custom Translations
    std::string mods;            // 根目录\Mods
    std::string settings;        // 根目录\Saving\Settings
    std::string version;         // streaming_assets\version.txt
    bool valid = false;
};

// hint 可以是：根目录 / exe 路径 / _Data 目录 / 任意子目录（向上找）
bool detect(const std::string& hint, Layout& out, std::string& err);

// 猜一个默认位置：上次记录 → Steam 注册表 + 库目录 → 常见安装路径
std::string guess_default();

// 游戏是否正在运行（改 DLL/设置前必须检查）
bool is_running();

// 安装状态（可卸载）
struct State {
    bool installed = false;
    std::string language_name;
    std::string translation_path;   // Custom Translations\<名字>.txt
    bool settings_existed = false;  // 安装前 LanguageSettings_2.txt 是否存在
    bool font_runtime = false;      // 是否装了字体/运行时 mod
    std::vector<std::string> files; // 我们铺过的文件（卸载时删）
};

// 按游戏自己的词条表过滤（优先 <游戏>_Data\Custom Translations\Example.txt，它是当前版本的完整表）。
// 不做这步的话，游戏每次启动都会弹「Your custom translation is out of date」。
std::string FilterTranslation(const std::string& raw_ini, size_t& dropped,
                              const std::string& game_root = std::string());

bool load_state(const Layout& l, State& st);
bool save_state(const Layout& l, const State& st);

// 写道 Custom Translations + 语言设置（ini_text 原样写入，UTF-8/带 BOM 都行）
bool install_translation(const Layout& l, const std::string& language_name, const std::string& ini_bytes,
                         bool overwrite, State& st, std::string& err);

// 铺字体 + 运行时 mod（dict_text 是合并后的 dict.json 内容；传空则不写）
bool install_font_runtime(const Layout& l, const std::string& runtime_dll, const std::string& font_ttf,
                          const std::string& dict_text, State& st, std::string& err);

// 只卸运行时 mod（字体/词表），保留本体翻译
bool uninstall_runtime(const Layout& l, State& st, std::string& err);

// 只卸本体翻译（删翻译文件 + 还原语言设置），保留运行时 mod
bool uninstall_translation(const Layout& l, State& st, std::string& err);

bool uninstall(const Layout& l, State& st, std::string& err);

// ---------------- 启动游戏 ----------------
struct LaunchInfo {
    void* process = nullptr;   // HANDLE
    unsigned long pid = 0;
};

// 直接跑 exe（不走 Steam 协议），工作目录设成游戏根目录
bool launch(const Layout& l, LaunchInfo& out, std::string& err);
bool process_alive(const LaunchInfo& info);
bool process_exit_code(const LaunchInfo& info, unsigned long& code);
void close_launch(LaunchInfo& info);

// 当前游戏语言（读 LanguageSettings_2.txt，读不到就空）
bool read_language(const Layout& l, std::string& code_name, bool& custom);
bool write_language(const Layout& l, const std::string& code_name, bool custom, std::string& err);

// 扫 Mods 下所有 .dll（深度 2：Mods\<目录>\*.dll 与 Mods\*.dll）
struct ModDll {
    std::string path;
    std::string name;    // 文件名（含 .dll）
    std::string folder;  // 所在文件夹名
    uint64_t size = 0;
    bool patched = false;  // 存在 .hanbak = 打过补丁
};
bool scan_mods(const Layout& l, std::vector<ModDll>& out, std::string& err);

}  // namespace game
