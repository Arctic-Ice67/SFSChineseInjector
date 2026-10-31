// 命令行入口：自测、批量、排查用（GUI 走同一套 han_core）。
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include "app.h"
#include "catalog.h"
#include "clr_image.h"
#include "fileio.h"
#include "fontcheck.h"
#include "inject.h"
#include "payload.h"
#include "inject_flow.h"
#include "game.h"
#include "ini.h"
#include "json.h"
#include "patch.h"
#include "text.h"

namespace {

void enable_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void usage() {
    std::printf(
        "SFS 汉化注入器 CLI\n"
        "  detect  [game]                 查看游戏目录/语言/安装状态/Mods 列表\n"
        "  install-base <game> [--force]  装本体汉化（Custom Translations + 语言设置）\n"
        "  uninstall-base <game>          卸载本体汉化并还原语言\n"
        "  runtime <game>                 装字体 + 运行时 mod + 合并词表成 dict.json\n"
        "  uninstall-runtime <game>       只卸运行时 mod\n"
        "  launch  <game>                 直接启动游戏（不走 Steam）\n"
        "  scan    <dll> [--all]          列出 #US 字面量\n"
        "  dump    <dll> <out.json>       导出字面量清单（造词表用）\n"
        "  patch   <dll> <catalog> [--dry-run]  静态打补丁（备份+自检+清单）\n"
        "  verify  <dll>                  用 .hanbak 校验结构完好\n"
        "  restore <dll>                  从 .hanbak 还原\n"
        "  coverage                       汉化包对英文基准的覆盖率\n"
        "  dict                           合并词表统计\n");
}

bool read_json(const std::string& path, json::Value& out) {
    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(path, raw, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return false;
    }
    std::string jerr;
    if (!json::parse(std::string(raw.begin(), raw.end()), out, jerr)) {
        std::printf("[错误] JSON 解析失败：%s\n", jerr.c_str());
        return false;
    }
    return true;
}

std::string file_text(const std::string& path) {
    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(path, raw, err)) return std::string();
    return std::string(raw.begin(), raw.end());
}

int cmd_detect(const std::string& hint) {
    std::string path = hint;
    if (path.empty()) {
        path = game::guess_default();
        std::printf("自动探测：%s\n", path.empty() ? "(没找到)" : path.c_str());
        if (path.empty()) return 1;
    }
    game::Layout l;
    std::string err;
    if (!game::detect(path, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::string version = file_text(l.version);
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();

    std::printf("游戏根目录 : %s\n", l.root.c_str());
    std::printf("版本       : %s\n", version.c_str());
    std::printf("Mods       : %s%s\n", l.mods.c_str(), fileio::is_dir(l.mods) ? "" : "  (不存在)");
    std::string lang;
    bool custom = false;
    if (game::read_language(l, lang, custom)) {
        std::printf("当前语言   : %s%s\n", lang.c_str(), custom ? "  (Custom)" : "  (官方)");
    } else {
        std::printf("当前语言   : (未记录)\n");
    }
    std::printf("游戏进程   : %s\n", game::is_running() ? "运行中" : "未运行");

    game::State st;
    game::load_state(l, st);
    std::printf("安装状态   : %s\n", st.installed ? "已装本体汉化" : "未装本体汉化");
    std::printf("运行时 mod : %s\n", st.font_runtime ? "已装" : "未装");
    if (st.installed) {
        std::printf("语言名     : %s\n翻译文件   : %s\n", st.language_name.c_str(), st.translation_path.c_str());
    }

    std::vector<game::ModDll> mods;
    std::string merr;
    if (game::scan_mods(l, mods, merr)) {
        std::printf("Mods DLL（%zu）：\n", mods.size());
        for (const auto& m : mods) {
            std::printf("  %-30s %8llu B %s\n", m.name.c_str(), static_cast<unsigned long long>(m.size),
                        m.patched ? "[已静态汉化]" : "");
        }
    }
    return 0;
}

int cmd_install_base(const std::string& hint, bool force) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    const std::string src = app::data_file("translations\\zh-CN.txt");
    const std::string raw = file_text(src);
    size_t dropped = 0;
    const std::string ini = game::FilterTranslation(raw, dropped, l.root);
    if (ini.empty()) {
        std::printf("[错误] 读不到翻译包 %s\n", src.c_str());
        return 1;
    }
    game::State st;
    game::load_state(l, st);
    if (!game::install_translation(l, "简体中文（汉化）", ini, force, st, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 本体汉化 → %s\n", st.translation_path.c_str());
    return 0;
}

int cmd_uninstall_base(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    game::State st;
    game::load_state(l, st);
    if (!game::uninstall_translation(l, st, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 已卸本体并还原语言设置（运行时保留）\n");
    return 0;
}

int cmd_uninstall_all(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    game::State st;
    game::load_state(l, st);
    game::uninstall_runtime(l, st, err);
    if (!game::uninstall(l, st, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 已卸载全部（本体 + 运行时 + 语言还原）\n");
    return 0;
}

int cmd_runtime(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    json::Value dict;
    catalog::MergeStats stats;
    std::string merr;
    if (!catalog::merge_dir(app::catalog_dir(), dict, stats, merr)) {
        std::printf("[错误] 合并词表失败：%s\n", merr.c_str());
        return 1;
    }
    std::printf("词表：%zu 文件 / 精确 %zu / 前缀 %zu / 跳过 %zu\n", stats.files, stats.exact, stats.prefix,
                stats.skipped);
    if (!merr.empty()) std::printf("[提示] %s\n", merr.c_str());

    game::State st;
    game::load_state(l, st);
    const std::string dll = fileio::join_path(app::runtime_dir(), "SFSHanRuntime.dll");
    const std::string font = fileio::join_path(app::fonts_dir(), "HarmonyOS_Sans_SC_Regular.ttf");
    if (!game::install_font_runtime(l, dll, font, json::write(dict, 2) + "\n", st, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 运行时 mod → %s\\Mods\\SFSHanRuntime\n", l.root.c_str());
    return 0;
}

int cmd_uninstall_runtime(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    game::State st;
    game::load_state(l, st);
    if (!game::uninstall_runtime(l, st, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 运行时 mod 已移除\n");
    return 0;
}

int cmd_launch(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    game::LaunchInfo info;
    if (!game::launch(l, info, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[启动] pid=%lu\n", info.pid);
    game::close_launch(info);
    return 0;
}

int cmd_scan(const std::string& path, bool all) {
    clr::Image img;
    std::string err;
    if (!img.load(path, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("程序集 %s | 方法 %zu | #US %zu 条 | ldstr %zu 处\n", img.assembly_name().c_str(),
                img.method_count(), img.strings().size(), img.ldstr_count());
    std::printf("%-4s %-8s %-6s %-5s %-5s %s\n", "#", "off", "bytes", "refs", "ident", "text");
    int shown = 0;
    for (size_t i = 0; i < img.strings().size(); ++i) {
        const auto& s = img.strings()[i];
        const bool ascii = !text::has_non_ascii(s.text) && !s.text.empty();
        const bool interesting = ascii && s.nbytes >= 6 && s.nbytes <= 400;
        if (!all && !interesting) continue;
        std::printf("%-4zu 0x%-6X %-6u %-5zu %-5s %s\n", i, s.heap_off, s.nbytes, s.sites.size(),
                    s.in_strings_heap ? "yes" : "-", text::utf16_to_utf8(s.text).c_str());
        if (++shown >= 500) break;
    }
    std::printf("（显示 %d 条）\n", shown);
    return 0;
}

int cmd_dump(const std::string& path, const std::string& out) {
    clr::Image img;
    std::string err;
    if (!img.load(path, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    json::Value root = json::make_object();
    json::set(root, "assembly", json::make_string(img.assembly_name()));
    json::set(root, "file", json::make_string(path));
    json::Value arr = json::make_array();
    for (size_t i = 0; i < img.strings().size(); ++i) {
        const auto& s = img.strings()[i];
        json::Value e = json::make_object();
        json::set(e, "index", json::make_number(static_cast<double>(i)));
        json::set(e, "off", json::make_number(s.heap_off));
        json::set(e, "nbytes", json::make_number(s.nbytes));
        json::set(e, "refs", json::make_number(static_cast<double>(s.sites.size())));
        json::set(e, "identifier", json::make_bool(s.in_strings_heap));
        json::set(e, "text", json::make_string(text::utf16_to_utf8(s.text)));
        json::Value owners = json::make_array();
        for (const auto& o : s.owners) json::push(owners, o);
        json::set(e, "owners", owners);
        json::push(arr, e);
    }
    json::set(root, "entries", arr);
    if (!fileio::write_all(out, json::write(root, 2) + "\n", err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("已写出 %s\n", out.c_str());
    return 0;
}

const char* risk_name(patch::Risk r) {
    switch (r) {
        case patch::Risk::Safe: return "可写";
        case patch::Risk::Caution: return "谨慎";
        case patch::Risk::Blocked: return "跳过";
    }
    return "?";
}

int cmd_patch(const std::string& dll, const std::string& catalog_path, bool dry) {
    clr::Image img;
    std::string err;
    if (!img.load(dll, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    json::Value cat;
    if (!read_json(catalog_path, cat)) return 1;
    patch::Plan plan;
    if (!patch::build_plan(img, cat, dll, plan, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("%s | 命中 %zu / 可写 %zu\n", img.assembly_name().c_str(), plan.items.size(), plan.use_count());
    for (const auto& it : plan.items) {
        std::printf("[%s] %s%s\n", risk_name(it.risk), it.src.c_str(), it.use ? "  → 写入" : "");
        if (!it.note.empty()) std::printf("        %s\n", it.note.c_str());
    }
    for (const auto& n : plan.notes) std::printf("[提示] %s\n", n.c_str());
    if (dry) return 0;
    patch::ApplyResult res;
    if (!patch::apply(img, plan, res, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 写入 %zu 条；备份 %s\n", res.patched, res.backup_path.c_str());
    size_t changed = 0;
    std::string verr;
    if (!patch::verify_against_backup(dll, res.backup_path, changed, verr)) {
        std::printf("[自检失败] %s\n", verr.c_str());
        return 1;
    }
    std::printf("[自检通过] %zu 条改写，结构与引用点一致\n", changed);
    return 0;
}

int cmd_verify(const std::string& dll) {
    std::string bak;
    if (!patch::has_backup(dll, bak)) {
        std::printf("[错误] 没有备份 %s\n", bak.c_str());
        return 1;
    }
    size_t changed = 0;
    std::string err;
    if (!patch::verify_against_backup(dll, bak, changed, err)) {
        std::printf("[失败] %s\n", err.c_str());
        return 1;
    }
    std::printf("[通过] %s：%zu 条改写\n", dll.c_str(), changed);
    return 0;
}

int cmd_restore(const std::string& dll) {
    std::string err;
    if (!patch::restore(dll, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 已还原\n");
    return 0;
}

int cmd_coverage() {
    auto load_doc = [](const std::string& path, ini::Doc& doc) {
        std::vector<uint8_t> raw;
        std::string err;
        if (!fileio::read_all(path, raw, err)) {
            std::printf("[错误] %s\n", err.c_str());
            return false;
        }
        std::string perr;
        if (!ini::parse(std::string(raw.begin(), raw.end()), doc, perr)) {
            std::printf("[错误] %s：%s\n", path.c_str(), perr.c_str());
            return false;
        }
        return true;
    };
    ini::Doc en, zh, official;
    if (!load_doc(app::data_file("translations\\English.txt"), en)) return 1;
    if (!load_doc(app::data_file("translations\\zh-CN.txt"), zh)) return 1;
    const bool has_official = load_doc(app::data_file("translations\\official-Chinese.txt"), official);
    const ini::Coverage c = ini::compare(en, zh);
    std::printf("英文基准 %zu 条 | 汉化包 %zu 条 | 覆盖 %zu (%.1f%%) | 缺失 %zu | 与英文相同 %zu\n", c.total,
                zh.entries.size(), c.present,
                c.total ? 100.0 * double(c.present) / double(c.total) : 0.0, c.empty,
                c.identical_to_english);
    if (has_official) {
        const ini::Coverage co = ini::compare(en, official);
        std::printf("官方中文对照：%zu/%zu\n", co.present, co.total);
    }
    for (size_t i = 0; i < c.missing.size() && i < 20; ++i) std::printf("  缺 %s\n", c.missing[i].c_str());
    return 0;
}

int cmd_dict() {
    json::Value dict;
    catalog::MergeStats st;
    std::string err;
    if (!catalog::merge_dir(app::catalog_dir(), dict, st, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("词表：%zu 文件 / 精确 %zu / 前缀 %zu / 跳过 %zu\n", st.files, st.exact, st.prefix, st.skipped);
    if (!err.empty()) std::printf("[提示] %s\n", err.c_str());
    return 0;
}

int cmd_fontcheck(const std::string& path) {
    std::vector<uint8_t> raw;
    std::string err;
    if (!fileio::read_all(app::data_file("translations\\zh-CN.txt"), raw, err)) {
        std::printf("[错误] 读不到汉化包：%s\n", err.c_str());
        return 1;
    }
    std::string text_all(raw.begin(), raw.end());
    if (text_all.size() >= 3 && static_cast<unsigned char>(text_all[0]) == 0xEF) text_all = text_all.substr(3);
    const std::u16string u = text::utf8_to_utf16(text_all);
    std::set<char16_t> uniq;
    for (char16_t c : u) {
        if (c >= 0x20) uniq.insert(c);
    }
    std::u16string needed(uniq.begin(), uniq.end());
    std::printf("汉化包用字：%zu 个不同字符\n", needed.size());

    fontcheck::Result r;
    if (!fontcheck::check(path, needed, r, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("字体 %s\n  字形 %zu | cmap %zu | 缺字 %zu\n", path.c_str(), r.glyphs, r.codepoints,
                r.missing.size());
    if (!r.missing.empty()) {
        std::u16string m;
        for (size_t i = 0; i < r.missing.size() && i < 40; ++i) m.push_back(static_cast<char16_t>(r.missing[i]));
        std::printf("  缺字示例：%s\n", text::utf16_to_utf8(m).c_str());
    }
    return r.missing.size() <= 30 ? 0 : 2;
}

// 只写 Mods\SFSHanRuntime\dict.json：游戏运行中也安全（运行时 mod 每 2 秒热重载）
int cmd_dict_write(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    json::Value dict;
    catalog::MergeStats stats;
    std::string merr;
    if (!catalog::merge_dir(app::catalog_dir(), dict, stats, merr)) {
        std::printf("[错误] 合并词表失败：%s\n", merr.c_str());
        return 1;
    }
    const std::string dir = fileio::join_path(l.mods, "SFSHanRuntime");
    if (!fileio::ensure_dir(dir)) {
        std::printf("[错误] 创建目录失败：%s\n", dir.c_str());
        return 1;
    }
    const std::string path = fileio::join_path(dir, "dict.json");
    if (!fileio::write_all(path, json::write(dict, 2) + "\n", err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 词表已写入 %s（精确 %zu 条 / 前缀 %zu 条）\n", path.c_str(), stats.exact, stats.prefix);
    if (!merr.empty()) std::printf("[提示] %s\n", merr.c_str());
    return 0;
}

// 取词表：优先 exe 内嵌载荷，其次 data\catalog 目录（开发时）
bool LoadDictJson(json::Value& dict, std::string& err) {
    std::vector<uint8_t> raw;
    if (payload::get("dict.json", raw)) {
        return json::parse(std::string(raw.begin(), raw.end()), dict, err);
    }
    catalog::MergeStats stats;
    return catalog::merge_dir(app::catalog_dir(), dict, stats, err);
}

// 取本体汉化文本
std::string LoadTranslationText() {
    std::vector<uint8_t> raw;
    if (payload::asset("translations/zh-CN.txt", raw))
        return std::string(raw.begin(), raw.end());
    std::vector<uint8_t> disk;
    std::string err;
    if (fileio::read_all(app::data_file("translations\\zh-CN.txt"), disk, err))
        return std::string(disk.begin(), disk.end());
    return std::string();
}

// 取内置字体并落到临时文件（Unity 的 Font 只能按路径加载）
std::string ExtractFontTemp(std::string& err) {
    const std::string path = payload::asset_to_temp("fonts/HarmonyOS_Sans_SC_Regular.ttf", ".ttf");
    if (path.empty()) err = "取内置字体失败";
    return path;
}
// 把合并后的词表写成 dict.json（打包单文件 exe 时用）
int cmd_dict_dump(const std::string& out_path) {
    json::Value dict;
    std::string err;
    if (!LoadDictJson(dict, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    if (!fileio::write_all(out_path, json::write(dict, 2) + "\n", err)) {
        std::printf("[错误] 写入失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 词表已写出 %s\n", out_path.c_str());
    return 0;
}
int cmd_payload() {
    std::printf("[载荷诊断] %s\n", payload::debug_info().c_str());
    if (!payload::present()) {
        std::printf("[载荷] exe 末尾没有载荷（开发模式）\n");
        return 1;
    }
    const auto names = payload::list();
    std::printf("[载荷] 共 %zu 个文件：\n", names.size());
    for (const auto& n : names) {
        std::vector<uint8_t> bytes;
        const bool ok = payload::get(n, bytes);
        std::printf("  %-42s %s %zu B\n", n.c_str(), ok ? "OK " : "FAIL", bytes.size());
    }
    return 0;
}
// 纯内存注入：不往游戏目录写任何文件，引导 DLL + 托管 DLL 直接在进程里跑
int cmd_inject(const std::string& hint) {
    game::Layout l;
    std::string err;
    if (!game::detect(hint, l, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[注入] 等游戏进程…\n");
    if (!inject_flow::Run(l, 60000, err)) {
        std::printf("[错误] %s\n", err.c_str());
        return 1;
    }
    std::printf("[完成] 已注入（内存中运行，磁盘无残留）\n");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    enable_utf8_console();
    if (argc < 2) {
        usage();
        return 0;
    }
    const std::string cmd = argv[1];
    auto has = [&](int i) { return argc > i; };
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        usage();
        return 0;
    }
    if (cmd == "detect") return cmd_detect(has(2) ? argv[2] : std::string());
    if (cmd == "coverage") return cmd_coverage();
    if (cmd == "dict") return cmd_dict();
    if (cmd == "install-base" && has(2)) {
        bool force = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--force") force = true;
        }
        return cmd_install_base(argv[2], force);
    }
    if (cmd == "uninstall-base" && has(2)) return cmd_uninstall_base(argv[2]);
    if (cmd == "uninstall-all" && has(2)) return cmd_uninstall_all(argv[2]);
    if (cmd == "runtime" && has(2)) return cmd_runtime(argv[2]);
    if (cmd == "uninstall-runtime" && has(2)) return cmd_uninstall_runtime(argv[2]);
    if (cmd == "dict-write" && has(2)) return cmd_dict_write(argv[2]);
    if (cmd == "inject" && has(2)) return cmd_inject(argv[2]);
    if (cmd == "dict-dump" && has(2)) return cmd_dict_dump(argv[2]);
    if (cmd == "payload") return cmd_payload();
    if (cmd == "launch" && has(2)) return cmd_launch(argv[2]);
    if (cmd == "scan" && has(2)) {
        bool all = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--all") all = true;
        }
        return cmd_scan(argv[2], all);
    }
    if (cmd == "dump" && has(3)) return cmd_dump(argv[2], argv[3]);
    if (cmd == "patch" && has(3)) {
        bool dry = false;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--dry-run") dry = true;
        }
        return cmd_patch(argv[2], argv[3], dry);
    }
    if (cmd == "verify" && has(2)) return cmd_verify(argv[2]);
    if (cmd == "fontcheck" && has(2)) return cmd_fontcheck(argv[2]);
    if (cmd == "restore" && has(2)) return cmd_restore(argv[2]);
    usage();
    return 2;
}
