// 词表合并：把 data\catalog\*.json 的 approved 条目并成运行时 mod 用的 dict.json
// （静态补丁也是读同一批词表，两边口味一致）。
#pragma once

#include <string>

#include "json.h"

namespace catalog {

struct MergeStats {
    size_t files = 0;
    size_t exact = 0;
    size_t prefix = 0;
    size_t inline_rules = 0;
    size_t skipped = 0;
};

// 扫描目录下所有 *.json 词表，输出 {"exact":{...},"prefix":[{from,to}...]}
bool merge_dir(const std::string& dir, json::Value& dict, MergeStats& stats, std::string& err);

// 读单个词表文件
bool load_file(const std::string& path, json::Value& out, std::string& err);

}  // namespace catalog
