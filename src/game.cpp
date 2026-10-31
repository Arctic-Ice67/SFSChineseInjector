#include "game.h"

#include <algorithm>
#include <cstdio>

#include <windows.h>
#include <tlhelp32.h>

#include "app.h"
#include "fileio.h"
#include "ini.h"
#include <set>
#include "json.h"
#include "patch.h"
#include "text.h"

namespace game {
namespace {

const char* kExeName = "Spaceflight Simulator.exe";
const char* kDataName = "Spaceflight Simulator_Data";
const char* kStateName = "SFSHanInjector.json";

std::string state_path(const Layout& l) { return fileio::join_path(l.settings, kStateName); }

// 从任意起点向上找，直到看见 exe 或 _Data
bool locate_from(const std::string& start, Layout& out) {
    std::string cur = fileio::absolute(start);
    for (int up = 0; up < 6 && !cur.empty(); ++up) {
        const std::string exe = fileio::join_path(cur, kExeName);
        const std::string data = fileio::join_path(cur, kDataName);
        if (fileio::exists(exe) || fileio::is_dir(data)) {
            Layout l;
            l.root = cur;
            l.exe = exe;
            l.data_dir = data;
            l.managed = fileio::join_path(data, "Managed");
            l.custom_trans = fileio::join_path(data, "Custom Translations");
            l.mods = fileio::join_path(cur, "Mods");
            l.settings = fileio::join_path(cur, "Saving\\Settings");
            l.version = fileio::join_path(data, "StreamingAssets\\version.txt");
            if (!fileio::exists(fileio::join_path(l.managed, "Assembly-CSharp.dll"))) {
                // 可能是别的目录名，继续往上找
                cur = fileio::parent_dir(cur);
                continue;
            }
            l.valid = true;
            out = l;
            return true;
        }
        cur = fileio::parent_dir(cur);
    }
    return false;
}

std::string read_text_best_effort(const std::string& path) {
    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(path, raw, err)) return std::string();
    return std::string(raw.begin(), raw.end());
}

std::string reg_string(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buf, &size) != ERROR_SUCCESS) {
        return std::string();
    }
    return text::wide_to_utf8(buf);
}

// 简易 vdf 解析：把 "path"		"D:\\SteamLibrary" 抓出来
void collect_vdf_paths(const std::string& text, std::vector<std::string>& out) {
    size_t pos = 0;
    while (true) {
        const size_t key = text.find("\"path\"", pos);
        if (key == std::string::npos) break;
        const size_t q1 = text.find('"', key + 6);
        if (q1 == std::string::npos) break;
        const size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string v = text.substr(q1 + 1, q2 - q1 - 1);
        for (size_t i = 0; i + 1 < v.size(); ++i) {
            if (v[i] == '\\' && v[i + 1] == '\\') {
                v.erase(i, 1);
            }
        }
        out.push_back(v);
        pos = q2 + 1;
    }
}

}  // namespace

bool detect(const std::string& hint, Layout& out, std::string& err) {
    if (hint.empty()) {
        err = "没有指定路径";
        return false;
    }
    if (!fileio::exists(hint) && !fileio::is_dir(hint)) {
        err = "路径不存在：" + hint;
        return false;
    }
    if (locate_from(hint, out)) return true;
    err = "这个目录里没找到 " + std::string(kExeName) + " / " + std::string(kDataName) +
          " / Managed\\Assembly-CSharp.dll";
    return false;
}

std::string guess_default() {
    std::vector<std::string> candidates;

    // 1) Steam 主目录
    std::string steam = reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!steam.empty()) {
        candidates.push_back(fileio::join_path(steam, "steamapps\\common\\Spaceflight Simulator"));
        // 2) 库目录
        std::vector<std::string> libs;
        collect_vdf_paths(read_text_best_effort(fileio::join_path(steam, "steamapps\\libraryfolders.vdf")), libs);
        for (const auto& lib : libs) {
            candidates.push_back(fileio::join_path(lib, "steamapps\\common\\Spaceflight Simulator"));
        }
    }
    // 3) 常见盘符
    static const char* kDrives[] = {"C:", "D:", "E:", "F:", "G:"};
    for (const char* d : kDrives) {
        candidates.push_back(std::string(d) + "\\Program Files (x86)\\Steam\\steamapps\\common\\Spaceflight Simulator");
        candidates.push_back(std::string(d) + "\\SteamLibrary\\steamapps\\common\\Spaceflight Simulator");
        candidates.push_back(std::string(d) + "\\Steam\\steamapps\\common\\Spaceflight Simulator");
        candidates.push_back(std::string(d) + "\\Games\\Spaceflight Simulator");
    }

    for (const auto& c : candidates) {
        Layout l;
        if (locate_from(c, l)) return l.root;
    }
    return std::string();
}

bool is_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            const std::string name = text::wide_to_utf8(pe.szExeFile);
            if (_stricmp(name.c_str(), kExeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::string FilterTranslation(const std::string& raw_ini, size_t& dropped, const std::string& game_root) {
    dropped = 0;
    // 优先用游戏自己生成的 Example.txt：它是「当前游戏版本」的完整词条表（星球名、Celestial Body 等都在里面）
    std::string ref_path;
    if (!game_root.empty()) {
        const std::string example = fileio::join_path(
            fileio::join_path(fileio::join_path(game_root, "Spaceflight Simulator_Data"), "Custom Translations"),
            "Example.txt");
        if (fileio::exists(example)) ref_path = example;
    }
    if (ref_path.empty()) ref_path = app::data_file("translations\\English.txt");

    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(ref_path, raw, err)) return raw_ini;
    std::string en_text(raw.begin(), raw.end());
    if (en_text.size() >= 3 && static_cast<unsigned char>(en_text[0]) == 0xEF) en_text = en_text.substr(3);
    ini::Doc en, zh;
    std::string perr;
    if (!ini::parse(en_text, en, perr)) return raw_ini;
    if (!ini::parse(raw_ini, zh, perr)) return raw_ini;
    if (en.entries.empty()) return raw_ini;
    const ini::Doc filtered = ini::filter_to_reference(zh, en, dropped);
    return ini::serialize(filtered);
}

bool load_state(const Layout& l, State& st) {
    st = State{};
    const std::string p = state_path(l);
    if (!fileio::exists(p)) return true;
    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(p, raw, err)) return false;
    json::Value v;
    std::string jerr;
    if (!json::parse(std::string(raw.begin(), raw.end()), v, jerr)) return false;
    st.installed = v.get_bool("installed", false);
    st.language_name = v.get_string("language_name");
    st.translation_path = v.get_string("translation_path");
    st.settings_existed = v.get_bool("settings_existed", false);
    st.font_runtime = v.get_bool("font_runtime", false);
    if (const json::Value* files = v.find("files"); files && files->is_array()) {
        for (const auto& f : files->items) st.files.push_back(f.as_string());
    }
    return true;
}

bool save_state(const Layout& l, const State& st) {
    fileio::ensure_dir(l.settings);
    json::Value v = json::make_object();
    json::set(v, "tool", json::make_string("SFSHanInjector"));
    json::set(v, "installed", json::make_bool(st.installed));
    json::set(v, "language_name", json::make_string(st.language_name));
    json::set(v, "translation_path", json::make_string(st.translation_path));
    json::set(v, "settings_existed", json::make_bool(st.settings_existed));
    json::set(v, "font_runtime", json::make_bool(st.font_runtime));
    json::Value arr = json::make_array();
    for (const auto& f : st.files) json::push(arr, f);
    json::set(v, "files", arr);
    std::string err;
    return fileio::write_all(state_path(l), json::write(v, 2) + "\n", err);
}

bool write_language(const Layout& l, const std::string& code_name, bool custom, std::string& err) {
    if (!fileio::ensure_dir(l.settings)) {
        err = "拿不到设置目录：" + l.settings;
        return false;
    }
    json::Value v = json::make_object();
    json::set(v, "CodeName", json::make_string(code_name));
    json::set(v, "Custom", json::make_bool(custom));
    const std::string p = fileio::join_path(l.settings, "LanguageSettings_2.txt");
    return fileio::write_all(p, json::write(v, 2), err);
}

bool read_language(const Layout& l, std::string& code_name, bool& custom) {
    code_name.clear();
    custom = false;
    const std::string p = fileio::join_path(l.settings, "LanguageSettings_2.txt");
    if (!fileio::exists(p)) return false;
    const std::string body = read_text_best_effort(p);
    json::Value v;
    std::string jerr;
    if (!json::parse(body, v, jerr)) return false;
    code_name = v.get_string("CodeName");
    custom = v.get_bool("Custom", false);
    return true;
}

bool install_translation(const Layout& l, const std::string& language_name, const std::string& ini_bytes,
                         bool overwrite, State& st, std::string& err) {
    if (!l.valid) {
        err = "游戏目录无效";
        return false;
    }
    if (is_running()) {
        err = "游戏正在运行，请先完全退出再安装";
        return false;
    }
    if (!fileio::ensure_dir(l.custom_trans)) {
        err = "创建目录失败：" + l.custom_trans;
        return false;
    }
    const std::string dst = fileio::join_path(l.custom_trans, language_name + ".txt");
    if (fileio::exists(dst) && !overwrite) {
        err = "已存在同名翻译文件：" + dst;
        return false;
    }
    if (!fileio::write_all(dst, ini_bytes.data(), ini_bytes.size(), err)) return false;

    // 语言设置：安装前备份一次（只备份第一次）
    const std::string settings_file = fileio::join_path(l.settings, "LanguageSettings_2.txt");
    const std::string settings_bak = settings_file + ".hanbak";
    if (fileio::exists(settings_file) && !fileio::exists(settings_bak)) {
        std::string cerr;
        fileio::copy_file(settings_file, settings_bak, false, cerr);
    }
    st.settings_existed = fileio::exists(settings_file);

    if (!write_language(l, language_name, true, err)) return false;

    st.installed = true;
    st.language_name = language_name;
    st.translation_path = dst;
    if (std::find(st.files.begin(), st.files.end(), dst) == st.files.end()) st.files.push_back(dst);
    save_state(l, st);
    return true;
}

bool install_font_runtime(const Layout& l, const std::string& runtime_dll, const std::string& font_ttf,
                          const std::string& dict_text, State& st, std::string& err) {
    if (!l.valid) {
        err = "游戏目录无效";
        return false;
    }
    if (is_running()) {
        err = "游戏正在运行，请先完全退出再安装";
        return false;
    }
    if (!fileio::exists(runtime_dll)) {
        err = "找不到运行时 mod：" + runtime_dll;
        return false;
    }
    const std::string dir = fileio::join_path(l.mods, "SFSHanRuntime");
    if (!fileio::ensure_dir(dir)) {
        err = "创建目录失败：" + dir;
        return false;
    }
    std::string cerr;
    const std::string dll_dst = fileio::join_path(dir, "SFSHanRuntime.dll");
    if (!fileio::copy_file(runtime_dll, dll_dst, true, cerr)) {
        err = "复制运行时 mod 失败：" + cerr;
        return false;
    }
    if (std::find(st.files.begin(), st.files.end(), dll_dst) == st.files.end()) st.files.push_back(dll_dst);

    if (!font_ttf.empty() && fileio::exists(font_ttf)) {
        const std::string font_dst = fileio::join_path(dir, "NotoSansSC-Bold.ttf");
        // 字体 8 MB：目标已存在且体积一致就跳过复制（否则每次开关都拷一遍，杀软还要扫）
        uint64_t src_size = 0, dst_size = 0;
        const bool same = fileio::file_size(font_ttf, src_size) && fileio::file_size(font_dst, dst_size) &&
                          src_size == dst_size && src_size != 0;
        if (!same) {
            if (!fileio::copy_file(font_ttf, font_dst, true, cerr)) {
                err = "复制字体失败：" + cerr;
                return false;
            }
        }
        if (std::find(st.files.begin(), st.files.end(), font_dst) == st.files.end()) st.files.push_back(font_dst);
    }
    if (!dict_text.empty()) {
        const std::string dict_dst = fileio::join_path(dir, "dict.json");
        if (!fileio::write_all(dict_dst, dict_text, err)) return false;
        if (std::find(st.files.begin(), st.files.end(), dict_dst) == st.files.end()) st.files.push_back(dict_dst);
    }

    // chars.txt：给运行时 mod 预热字体图集用（含所有会出现的中文字符）。
    // 少了它，动态字体图集逐条加字形会漏，个别字就显示成方块。
    {
        std::set<char16_t> uni;
        auto collect = [&uni](const std::string& s) {
            const std::u16string u = text::utf8_to_utf16(s);
            for (char16_t c : u)
                if (c >= 0x80) uni.insert(c);
        };
        const std::string tpath = fileio::join_path(
            fileio::join_path(fileio::join_path(l.root, "Spaceflight Simulator_Data"), "Custom Translations"),
            "简体中文（汉化）.txt");
        std::vector<uint8_t> raw;
        std::string rerr;
        if (fileio::read_all(tpath, raw, rerr)) collect(std::string(raw.begin(), raw.end()));
        collect(dict_text);
        if (!uni.empty()) {
            std::u16string out;
            out.reserve(uni.size());
            for (char16_t c : uni) out.push_back(c);
            const std::string chars_dst = fileio::join_path(dir, "chars.txt");
            if (!fileio::write_all(chars_dst, text::utf16_to_utf8(out), err)) return false;
            if (std::find(st.files.begin(), st.files.end(), chars_dst) == st.files.end())
                st.files.push_back(chars_dst);
        }
    }
    st.font_runtime = true;
    save_state(l, st);
    return true;
}

bool uninstall_runtime(const Layout& l, State& st, std::string& err) {
    if (!l.valid) {
        err = "游戏目录无效";
        return false;
    }
    if (is_running()) {
        err = "游戏正在运行，请先完全退出";
        return false;
    }
    const std::string dir = fileio::join_path(l.mods, "SFSHanRuntime");
    static const char* kNames[] = {"SFSHanRuntime.dll", "dict.json", "NotoSansSC-Bold.ttf",
                                    "SFSHanRuntime.log"};
    for (const char* n : kNames) {
        std::string ierr;
        const std::string p = fileio::join_path(dir, n);
        fileio::remove_file(p, ierr);
        st.files.erase(std::remove(st.files.begin(), st.files.end(), p), st.files.end());
    }
    std::vector<fileio::DirEntry> left;
    std::string lerr;
    if (fileio::list_dir(dir, left, lerr) && left.empty()) {
        RemoveDirectoryW(text::utf8_to_wide(dir).c_str());
    }
    st.font_runtime = false;
    save_state(l, st);
    return true;
}

bool uninstall_translation(const Layout& l, State& st, std::string& err) {
    if (!l.valid) {
        err = "游戏目录无效";
        return false;
    }
    if (is_running()) {
        err = "游戏正在运行，请先完全退出";
        return false;
    }
    // 还原语言设置
    const std::string settings_file = fileio::join_path(l.settings, "LanguageSettings_2.txt");
    const std::string settings_bak = settings_file + ".hanbak";
    if (fileio::exists(settings_bak)) {
        std::string cerr;
        fileio::copy_file(settings_bak, settings_file, true, cerr);
        fileio::remove_file(settings_bak, cerr);
    } else {
        std::string ierr;
        fileio::remove_file(settings_file, ierr);
    }
    // 删翻译文件（只删我们记录过的那个）
    if (!st.translation_path.empty()) {
        std::string ierr;
        fileio::remove_file(st.translation_path, ierr);
        st.files.erase(std::remove(st.files.begin(), st.files.end(), st.translation_path), st.files.end());
    }
    st.installed = false;
    st.language_name.clear();
    st.translation_path.clear();
    save_state(l, st);
    return true;
}

bool launch(const Layout& l, LaunchInfo& out, std::string& err) {
    out = LaunchInfo{};
    if (!l.valid) {
        err = "游戏目录无效";
        return false;
    }
    if (!fileio::exists(l.exe)) {
        err = "找不到游戏可执行文件：" + l.exe;
        return false;
    }
    if (is_running()) {
        err = "游戏已经在运行了";
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = text::utf8_to_wide("\"" + l.exe + "\"");
    std::wstring cwd = text::utf8_to_wide(l.root);
    if (!CreateProcessW(text::utf8_to_wide(l.exe).c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        cwd.c_str(), &si, &pi)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "启动失败（Win32 错误 %lu）", static_cast<unsigned long>(GetLastError()));
        err = buf;
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    return true;
}

bool process_alive(const LaunchInfo& info) {
    if (!info.process) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(info.process), &code)) return false;
    return code == STILL_ACTIVE;
}

bool process_exit_code(const LaunchInfo& info, unsigned long& code) {
    if (!info.process) return false;
    DWORD c = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(info.process), &c)) return false;
    if (c == STILL_ACTIVE) return false;
    code = c;
    return true;
}

void close_launch(LaunchInfo& info) {
    if (info.process) {
        CloseHandle(static_cast<HANDLE>(info.process));
        info.process = nullptr;
    }
    info.pid = 0;
}

bool uninstall(const Layout& l, State& st, std::string& err) {
    if (!l.valid) {
        err = "游戏目录无效";
        return false;
    }
    if (is_running()) {
        err = "游戏正在运行，请先完全退出再卸载";
        return false;
    }
    // 1) 还原语言设置
    const std::string settings_file = fileio::join_path(l.settings, "LanguageSettings_2.txt");
    const std::string settings_bak = settings_file + ".hanbak";
    if (fileio::exists(settings_bak)) {
        std::string cerr;
        fileio::copy_file(settings_bak, settings_file, true, cerr);
        fileio::remove_file(settings_bak, cerr);
    } else {
        std::string ierr;
        fileio::remove_file(settings_file, ierr);
    }
    // 2) 删我们铺的文件
    for (const auto& f : st.files) {
        std::string ierr;
        fileio::remove_file(f, ierr);
        fileio::remove_file(f + ".hanbak", ierr);
    }
    // 3) 删空目录
    const std::string rt = fileio::join_path(l.mods, "SFSHanRuntime");
    std::vector<fileio::DirEntry> entries;
    std::string lerr;
    if (fileio::list_dir(rt, entries, lerr) && entries.empty()) {
        RemoveDirectoryW(text::utf8_to_wide(rt).c_str());
    }
    st = State{};
    save_state(l, st);
    return true;
}

bool scan_mods(const Layout& l, std::vector<ModDll>& out, std::string& err) {
    out.clear();
    if (!fileio::is_dir(l.mods)) {
        err = "没有 Mods 目录：" + l.mods;
        return false;
    }
    auto add = [&](const std::string& full, const std::string& folder, uint64_t size) {
        if (fileio::file_ext(full) != ".dll") return;
        if (text::ends_with(text::lower_ascii(full), ".hanbak")) return;
        ModDll m;
        m.path = full;
        m.name = fileio::file_name(full);
        m.folder = folder;
        m.size = size;
        m.patched = fileio::exists(patch::backup_path_of(full));
        out.push_back(std::move(m));
    };
    std::vector<fileio::DirEntry> top;
    if (!fileio::list_dir(l.mods, top, err)) return false;
    for (const auto& e : top) {
        if (!e.dir) {
            add(fileio::join_path(l.mods, e.name), std::string(), e.size);
            continue;
        }
        std::vector<fileio::DirEntry> inner;
        std::string ierr;
        if (fileio::list_dir(fileio::join_path(l.mods, e.name), inner, ierr)) {
            for (const auto& ie : inner) {
                if (ie.dir) continue;
                add(fileio::join_path(fileio::join_path(l.mods, e.name), ie.name), e.name, ie.size);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const ModDll& a, const ModDll& b) { return a.path < b.path; });
    return true;
}

}  // namespace game
