#include "ini.h"

#include <algorithm>

#include "text.h"

namespace ini {
namespace {

std::string strip_cr(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

}  // namespace

bool parse(const std::string& text, Doc& out, std::string& err) {
    out = Doc{};
    // 去 UTF-8 BOM
    size_t start = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        start = 3;
    }
    std::string group;
    size_t pos = start;
    int line_no = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        std::string line = strip_cr(text.substr(pos, end - pos));
        pos = end + 1;
        ++line_no;
        const std::string trimmed = text::trim(line);
        if (trimmed.empty()) {
            if (nl == std::string::npos) break;
            continue;
        }
        if (trimmed[0] == '#' || trimmed[0] == ';') {
            if (nl == std::string::npos) break;
            continue;
        }
        if (trimmed[0] == '[') {
            const size_t rb = trimmed.find(']');
            if (rb == std::string::npos) {
                err = "第 " + std::to_string(line_no) + " 行的分组缺少 ]";
                return false;
            }
            group = text::trim(trimmed.substr(1, rb - 1));
            if (std::find(out.groups.begin(), out.groups.end(), group) == out.groups.end()) {
                out.groups.push_back(group);
            }
            if (nl == std::string::npos) break;
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            // 没有 = 的行忽略（SFS 自己也容错）
            if (nl == std::string::npos) break;
            continue;
        }
        Entry e;
        e.group = group;
        e.key = text::trim(trimmed.substr(0, eq));
        e.value = text::trim(trimmed.substr(eq + 1));
        if (!e.key.empty()) out.entries.push_back(std::move(e));
        if (nl == std::string::npos) break;
    }
    if (out.entries.empty()) {
        err = "翻译文件里一个词条都没解析到";
        return false;
    }
    return true;
}

const Entry* find(const Doc& d, const std::string& group, const std::string& key) {
    return d.find(group, key);
}

std::string serialize(const Doc& doc) {
    std::string out;
    std::string cur;
    for (const auto& e : doc.entries) {
        if (e.group != cur) {
            if (!out.empty()) out += "\n";
            out += "[" + e.group + "]\n";
            cur = e.group;
        }
        out += "  " + e.key + "=" + e.value + "\n";
    }
    return out;
}

Doc filter_to_reference(const Doc& translated, const Doc& reference, size_t& dropped) {
    Doc out;
    dropped = 0;
    out.groups = translated.groups;
    for (const auto& e : translated.entries) {
        if (reference.find(e.group, e.key)) {
            out.entries.push_back(e);
        } else {
            ++dropped;
        }
    }
    return out;
}

Coverage compare(const Doc& english, const Doc& translated) {    Coverage c;
    c.total = english.entries.size();
    for (const auto& en : english.entries) {
        const Entry* tr = translated.find(en.group, en.key);
        const std::string label = en.group + " / " + en.key;
        if (!tr || tr->value.empty()) {
            ++c.empty;
            if (c.missing.size() < 500) c.missing.push_back(label);
            continue;
        }
        ++c.present;
        if (tr->value == en.value) ++c.identical_to_english;
    }
    return c;
}

}  // namespace ini
