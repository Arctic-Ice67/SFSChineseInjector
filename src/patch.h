// 补丁计划 / 应用 / 还原。
//
// 安全策略：
//   * 只翻词表里显式列出的字符串（按原文精确匹配），不做「猜着翻」
//   * 应用前做长度预检；应用后跑 clr::Image::self_check
//   * 原文件已有 .hanbak 则不覆盖它（记录的是最早的原件）
//   * 清单写 .hanpatch.json，可逐条追溯
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "clr_image.h"
#include "json.h"

namespace patch {

enum class Risk { Safe, Caution, Blocked };

struct PlanItem {
    size_t index = 0;        // #US 条目下标
    std::string src;         // 原文（UTF-8）
    std::u16string zh;       // 译文
    bool use = false;        // 是否要写
    Risk risk = Risk::Caution;
    std::string note;        // 不能写的原因 / 备注
    uint32_t nbytes = 0;     // 原文内容字节数（长度上限）
    size_t refs = 0;
};

struct Plan {
    std::string dll_path;
    std::string assembly;
    std::vector<PlanItem> items;
    std::vector<std::string> notes;  // 词表里有、但 DLL 里找不到的原文
    size_t use_count() const;
};

// catalog 结构见 data/catalog/README 说明
bool build_plan(clr::Image& img, const json::Value& catalog, const std::string& dll_path, Plan& plan,
                std::string& err);

struct ApplyResult {
    size_t patched = 0;
    std::string backup_path;
    std::string manifest_path;
    std::string sha_before;
    std::string sha_after;
};

// 写 .hanbak（若不存在）+ 应用 + 写清单；失败时不留下半成品
bool apply(clr::Image& img, const Plan& plan, ApplyResult& result, std::string& err);

// 从 .hanbak 还原（不存在则报错）
bool restore(const std::string& dll_path, std::string& err);
bool has_backup(const std::string& dll_path, std::string& backup_path);
bool has_manifest(const std::string& dll_path, std::string& manifest_path);

// 拿备份当原件，校验当前 DLL 是否「只改了 #US 内容」且结构完好。
// 这是打完补丁后最重要的一道闸门（能抓出补丁把 metadata 搞坏的情况）。
bool verify_against_backup(const std::string& dll_path, const std::string& backup_path, size_t& changed_entries,
                           std::string& err);

std::string backup_path_of(const std::string& dll_path);
std::string manifest_path_of(const std::string& dll_path);

}  // namespace patch
