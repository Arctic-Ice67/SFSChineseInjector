#include "inject_flow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <set>
#include <vector>

#include "app.h"
#include "catalog.h"
#include "fileio.h"
#include "inject.h"
#include "json.h"
#include "payload.h"
#include "text.h"

namespace inject_flow {

std::string TranslationText() {
    std::vector<uint8_t> raw;
    if (payload::asset("translations/zh-CN.txt", raw))
        return std::string(raw.begin(), raw.end());
    std::vector<uint8_t> disk;
    std::string err;
    if (fileio::read_all(app::data_file("translations\\zh-CN.txt"), disk, err))
        return std::string(disk.begin(), disk.end());
    return std::string();
}

bool DictJson(std::string& out, std::string& err) {
    std::vector<uint8_t> raw;
    if (payload::get("dict.json", raw)) {
        out.assign(raw.begin(), raw.end());
        return true;
    }
    json::Value dict;
    catalog::MergeStats stats;
    if (!catalog::merge_dir(app::catalog_dir(), dict, stats, err)) return false;
    out = json::write(dict, 2);
    return true;
}

namespace {

// 把会出现的中文字符集抽出来（给运行时预热字体图集）
std::string BuildChars(const std::string& translation_text) {
    std::set<char16_t> uni;
    const std::u16string u = text::utf8_to_utf16(translation_text);
    for (char16_t ch : u)
        if (ch >= 0x80) uni.insert(ch);
    std::u16string out;
    out.reserve(uni.size());
    for (char16_t ch : uni) out.push_back(ch);
    return text::utf16_to_utf8(out);
}

}  // namespace

bool Run(const game::Layout& layout, uint32_t wait_ms, std::string& err) {
    if (!layout.valid) {
        err = "游戏目录无效";
        return false;
    }

    // 等游戏进程
    uint32_t pid = 0;
    const uint32_t step = 500;
    for (uint32_t waited = 0; waited <= wait_ms; waited += step) {
        pid = inject::find_process(L"Spaceflight Simulator.exe");
        if (pid != 0) break;
        ::Sleep(step);
    }
    if (pid == 0) {
        err = "等不到游戏进程";
        return false;
    }

    // 等 Mono 起来（不然 mono_get_root_domain 还是空的，注入会失败）
    if (!inject::wait_for_module(pid, L"mono-2.0-bdwgc.dll", wait_ms)) {
        err = "等不到 Mono 运行时";
        return false;
    }
    ::Sleep(4000);   // 再给游戏 UI 一点时间

    std::vector<uint8_t> boot, managed;
    if (!payload::asset("inject/SFSHanBootstrap.dll", boot)) {
        err = "取不到引导 DLL";
        return false;
    }
    if (!payload::asset("runtime/SFSHanRuntime.dll", managed)) {
        err = "取不到运行时 DLL";
        return false;
    }

    std::string dict_json;
    if (!DictJson(dict_json, err)) return false;

    const std::string chars = BuildChars(TranslationText());

    const std::string font_tmp =
        payload::asset_to_temp("fonts/HarmonyOS_Sans_SC_Regular.ttf", ".ttf");
    if (font_tmp.empty()) {
        err = "取不到内置字体";
        return false;
    }

    json::Value cfg = json::make_object();
    json::set(cfg, "font", json::make_string(font_tmp));
    json::set(cfg, "fontTemp", json::make_string(font_tmp));
    json::set(cfg, "chars", json::make_string(chars));
    // dict 是 JSON 对象，直接嵌进去（运行时按对象解析）
    json::Value dict_val;
    std::string jerr;
    if (json::parse(dict_json, dict_val, jerr)) {
        json::set(cfg, "dict", dict_val);
    } else {
        json::set(cfg, "dict", json::make_object());
    }

    const std::wstring mono = text::utf8_to_wide(fileio::join_path(
        fileio::join_path(layout.root, "MonoBleedingEdge"), "EmbedRuntime\\mono-2.0-bdwgc.dll"));

    uint32_t code = 0;
    const bool ok = inject::inject_runtime(pid, boot, managed, mono, json::write(cfg), std::wstring(),
                                           &code, err);
    ::Sleep(2500);              // 等运行时把字体读进内存
    payload::cleanup_temp();    // 删掉临时字体
    if (!ok) return false;
    if (code != 0) {
        err = "远程线程退出码 " + std::to_string(code);
        return false;
    }
    return true;
}

}  // namespace inject_flow
