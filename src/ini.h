// SFS 翻译文件用的 INI 子集解析（[Group] + Key=Value），用于覆盖率统计与差异对比。
#pragma once

#include <string>
#include <vector>

namespace ini {

struct Entry {
    std::string group;
    std::string key;
    std::string value;
};

struct Doc {
    std::vector<Entry> entries;
    std::vector<std::string> groups;

    const Entry* find(const std::string& group, const std::string& key) const {
        for (const auto& e : entries) {
            if (e.group == group && e.key == key) return &e;
        }
        return nullptr;
    }
};

bool parse(const std::string& text, Doc& out, std::string& err);
const Entry* find(const Doc& d, const std::string& group, const std::string& key);

// 按分组顺序写回 INI 文本（用于「按游戏真实词条过滤后」再落盘）
std::string serialize(const Doc& doc);

// 只保留 reference 里存在的词条（游戏提示 custom translation out of date 就是因为我们多了词条）
Doc filter_to_reference(const Doc& translated, const Doc& reference, size_t& dropped);

struct Coverage {
    size_t total = 0;
    size_t present = 0;
    size_t empty = 0;
    size_t identical_to_english = 0;  // 译文与英文一样（多半没翻）
    std::vector<std::string> missing; // "Group / Key"
};

// 以 english 为基准，统计 translated 的覆盖情况
Coverage compare(const Doc& english, const Doc& translated);

}  // namespace ini
