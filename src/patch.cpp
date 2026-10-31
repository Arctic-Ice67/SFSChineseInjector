#include "patch.h"

#include <algorithm>
#include <ctime>

#include "fileio.h"
#include "sha256.h"
#include "text.h"

namespace patch {
namespace {

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string to_hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

}  // namespace

size_t Plan::use_count() const {
    size_t n = 0;
    for (const auto& i : items) {
        if (i.use) ++n;
    }
    return n;
}

std::string backup_path_of(const std::string& dll_path) { return dll_path + ".hanbak"; }
std::string manifest_path_of(const std::string& dll_path) { return dll_path + ".hanpatch.json"; }

bool has_backup(const std::string& dll_path, std::string& backup_path) {
    backup_path = backup_path_of(dll_path);
    return fileio::exists(backup_path);
}

bool has_manifest(const std::string& dll_path, std::string& manifest_path) {
    manifest_path = manifest_path_of(dll_path);
    return fileio::exists(manifest_path);
}

bool build_plan(clr::Image& img, const json::Value& catalog, const std::string& dll_path, Plan& plan,
                std::string& err) {
    plan = Plan{};
    plan.dll_path = dll_path;
    plan.assembly = img.assembly_name();

    const json::Value* entries = catalog.find("entries");
    if (entries && !entries->is_object() && !entries->is_array()) {
        err = "词表格式错误：entries 必须是对象或数组";
        return false;
    }

    // 建立 原文 -> {译文, state} 映射
    struct Tr {
        std::string zh;
        std::string state;
        std::string note;
    };
    std::vector<std::pair<std::string, Tr>> table;
    if (entries && entries->is_object()) {
        for (const auto& kv : entries->fields) {
            Tr t;
            if (kv.second.is_object()) {
                t.zh = kv.second.get_string("zh");
                t.state = kv.second.get_string("state", "approved");
                t.note = kv.second.get_string("note");
            } else {
                t.zh = kv.second.as_string();
            }
            table.emplace_back(kv.first, t);
        }
    } else if (entries && entries->is_array()) {
        for (const auto& e : entries->items) {
            const std::string src = e.get_string("src");
            if (src.empty()) continue;
            Tr t;
            t.zh = e.get_string("zh");
            t.state = e.get_string("state", "approved");
            t.note = e.get_string("note");
            table.emplace_back(src, t);
        }
    }

    if (table.empty()) {
        err = "词表里没有 entries（空词表）";
        return false;
    }

    // skip 列表：明确不翻
    std::vector<std::string> skips;
    if (const json::Value* sk = catalog.find("skip"); sk && sk->is_array()) {
        for (const auto& s : sk->items) skips.push_back(s.as_string());
    }

    std::vector<bool> matched(table.size(), false);

    for (size_t i = 0; i < img.strings().size(); ++i) {
        const auto& us = img.strings()[i];
        if (us.sites.empty()) continue;  // 没被引用 = 死代码
        const std::string src = text::utf16_to_utf8(us.text);
        if (src.empty()) continue;
        if (std::find(skips.begin(), skips.end(), src) != skips.end()) continue;

        size_t hit = table.size();
        for (size_t k = 0; k < table.size(); ++k) {
            if (table[k].first == src) {
                hit = k;
                break;
            }
        }
        if (hit == table.size()) continue;
        matched[hit] = true;

        PlanItem item;
        item.index = i;
        item.src = src;
        item.nbytes = us.nbytes;
        item.refs = us.sites.size();
        item.note = table[hit].second.note;
        item.zh = text::utf8_to_utf16(table[hit].second.zh);

        const std::string& state = table[hit].second.state;
        if (table[hit].second.zh.empty()) {
            item.risk = Risk::Blocked;
            item.note = item.note.empty() ? "词表里没有译文" : item.note;
        } else if (state == "skip") {
            item.risk = Risk::Blocked;
            item.note = item.note.empty() ? "词表标记为不翻" : item.note;
        } else if (item.zh.size() * 2 > us.nbytes) {
            item.risk = Risk::Blocked;
            char buf[192];
            std::snprintf(buf, sizeof(buf), "译文太长：%zu 字节 > 原文 %u 字节，请改短", item.zh.size() * 2,
                          us.nbytes);
            item.note = buf;
        } else if (us.in_strings_heap && !text::has_non_ascii(us.text)) {
            // 原文同时是标识符名：翻了有风险，但仍然允许（词表显式列出即视为已确认）
            item.risk = Risk::Caution;
            if (item.note.empty()) item.note = "该文本同时出现在标识符堆里，注意确认";
            item.use = state == "approved";
        } else {
            item.risk = Risk::Safe;
            item.use = state == "approved";
        }
        if (state == "draft") item.use = false;
        plan.items.push_back(std::move(item));
    }

    for (size_t k = 0; k < table.size(); ++k) {
        if (!matched[k]) {
            plan.notes.push_back("词表条目在 DLL 里找不到：" + table[k].first);
        }
    }
    return true;
}

bool apply(clr::Image& img, const Plan& plan, ApplyResult& result, std::string& err) {
    size_t to_apply = 0;
    for (const auto& it : plan.items) {
        if (it.use) ++to_apply;
    }
    if (to_apply == 0) {
        err = "没有可应用的译文（词表里没有 approved 条目或全部超长）";
        return false;
    }

    // 1) 备份原件（已有备份则保留最早的）
    const std::string bak = backup_path_of(plan.dll_path);
    const bool created_backup = !fileio::exists(bak);
    if (created_backup) {
        std::string cp_err;
        if (!fileio::copy_file(plan.dll_path, bak, false, cp_err)) {
            err = "备份原件失败：" + cp_err;
            return false;
        }
    }
    // 中途失败：把这次新建的备份撤掉，别留下「看起来打过补丁」的痕迹
    auto fail = [&](const std::string& msg) {
        if (created_backup) {
            std::string ignore;
            fileio::remove_file(bak, ignore);
        }
        err = msg;
        return false;
    };
    result.backup_path = bak;
    result.sha_before = sha256util::hex_digest(img.bytes().data(), img.bytes().size());

    // 2) 逐条写入
    size_t done = 0;
    for (const auto& it : plan.items) {
        if (!it.use) continue;
        std::string rerr;
        if (!img.replace(it.index, it.zh, rerr)) {
            return fail("写入失败（" + it.src + "）：" + rerr);
        }
        ++done;
    }

    // 3) 结构自检
    std::string cerr;
    if (!img.self_check(cerr)) {
        return fail("自检失败，已放弃写盘：" + cerr);
    }

    // 4) 落盘（先写临时文件再替换，避免写一半把游戏文件搞坏）
    const std::string tmp = plan.dll_path + ".hantmp";
    std::string werr;
    if (!img.save_as(tmp, werr)) {
        return fail("写入临时文件失败：" + werr);
    }
    if (!fileio::rename_file(tmp, plan.dll_path, werr)) {
        fileio::remove_file(tmp, werr);
        return fail("替换文件失败：" + werr);
    }

    result.patched = done;
    result.sha_after = sha256util::hex_digest(img.bytes().data(), img.bytes().size());

    // 5) 清单
    json::Value man = json::make_object();
    json::set(man, "tool", json::make_string("SFSHanInjector"));
    json::set(man, "tool_version", json::make_string("0.1.0"));
    json::set(man, "time", json::make_string(now_iso()));
    json::set(man, "dll", json::make_string(plan.dll_path));
    json::set(man, "assembly", json::make_string(plan.assembly));
    json::set(man, "sha256_before", json::make_string(result.sha_before));
    json::set(man, "sha256_after", json::make_string(result.sha_after));
    json::Value arr = json::make_array();
    for (const auto& it : plan.items) {
        if (!it.use) continue;
        json::Value e = json::make_object();
        json::set(e, "index", json::make_number(static_cast<double>(it.index)));
        json::set(e, "src", json::make_string(it.src));
        json::set(e, "zh", json::make_string(text::utf16_to_utf8(it.zh)));
        json::set(e, "nbytes", json::make_number(it.nbytes));
        json::push(arr, e);
    }
    json::set(man, "entries", arr);
    result.manifest_path = manifest_path_of(plan.dll_path);
    if (!fileio::write_all(result.manifest_path, json::write(man, 2) + "\n", werr)) {
        err = "应用成功但写清单失败：" + werr;
        return false;
    }
    return true;
}

bool restore(const std::string& dll_path, std::string& err) {
    const std::string bak = backup_path_of(dll_path);
    if (!fileio::exists(bak)) {
        err = "没有备份文件，无法还原：" + bak;
        return false;
    }
    const std::string tmp = dll_path + ".hanrestore";
    if (!fileio::copy_file(bak, tmp, true, err)) return false;
    if (!fileio::rename_file(tmp, dll_path, err)) {
        fileio::remove_file(tmp, err);
        return false;
    }
    std::string ierr;
    fileio::remove_file(bak, ierr);  // 备份用完即删
    return true;
}

bool verify_against_backup(const std::string& dll_path, const std::string& backup_path, size_t& changed_entries,
                           std::string& err) {
    changed_entries = 0;
    clr::Image orig;
    clr::Image now;
    std::string e1, e2;
    if (!orig.load(backup_path, e1)) {
        err = "读不了备份：" + e1;
        return false;
    }
    if (!now.load(dll_path, e2)) {
        err = "读不了当前 DLL（结构可能已损坏）：" + e2;
        return false;
    }
    if (orig.bytes().size() != now.bytes().size()) {
        err = "文件大小与备份不一致：备份 " + std::to_string(orig.bytes().size()) + " 字节，现在 " +
              std::to_string(now.bytes().size()) + " 字节";
        return false;
    }
    if (orig.strings().size() != now.strings().size()) {
        err = "#US 条目数与备份不一致";
        return false;
    }
    if (orig.method_count() != now.method_count()) {
        err = "方法数与备份不一致";
        return false;
    }
    for (size_t i = 0; i < orig.strings().size(); ++i) {
        const auto& a = orig.strings()[i];
        const auto& b = now.strings()[i];
        if (a.heap_off != b.heap_off || a.nbytes != b.nbytes || a.content_at != b.content_at ||
            a.sites.size() != b.sites.size()) {
            err = "第 " + std::to_string(i) + " 条字面量的偏移/长度/引用点被改动";
            return false;
        }
        if (a.text != b.text) ++changed_entries;
    }
    // 所有差异必须落在内容区/flag 字节里
    const auto& ob = orig.bytes();
    const auto& nb = now.bytes();
    size_t i = 0;
    while (i < ob.size()) {
        if (ob[i] == nb[i]) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < ob.size() && ob[i] != nb[i]) ++i;
        bool inside = false;
        for (const auto& s : orig.strings()) {
            if (start >= s.content_at && i <= static_cast<uint64_t>(s.flag_at) + 1) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "改动落在 #US 之外：0x%zX..0x%zX", start, i);
            err = buf;
            return false;
        }
    }
    return true;
}

}  // namespace patch
