#include "catalog.h"

#include <algorithm>

#include "fileio.h"
#include "text.h"

namespace catalog {
namespace {

bool state_enabled(const json::Value& v) {
    const std::string st = v.get_string("state", "approved");
    return st != "skip" && st != "draft";
}

}  // namespace

bool load_file(const std::string& path, json::Value& out, std::string& err) {
    std::vector<uint8_t> raw;
    if (!fileio::read_all(path, raw, err)) return false;
    std::string jerr;
    if (!json::parse(std::string(raw.begin(), raw.end()), out, jerr)) {
        err = path + "：" + jerr;
        return false;
    }
    return true;
}

bool merge_dir(const std::string& dir, json::Value& dict, MergeStats& stats, std::string& err) {
    dict = json::make_object();
    stats = MergeStats{};
    if (!fileio::is_dir(dir)) {
        err = "词表目录不存在：" + dir;
        return false;
    }
    std::vector<fileio::DirEntry> entries;
    if (!fileio::list_dir(dir, entries, err)) return false;
    std::sort(entries.begin(), entries.end(),
              [](const fileio::DirEntry& a, const fileio::DirEntry& b) { return a.name < b.name; });

    json::Value exact = json::make_object();
    json::Value prefix = json::make_array();
    json::Value inline_rules = json::make_array();

    for (const auto& e : entries) {
        if (e.dir) continue;
        if (fileio::file_ext(e.name) != ".json") continue;
        const std::string path = fileio::join_path(dir, e.name);
        json::Value doc;
        std::string lerr;
        if (!load_file(path, doc, lerr)) {
            err += lerr + "\n";
            continue;
        }
        ++stats.files;
        if (const json::Value* items = doc.find("entries")) {
            if (items->is_object()) {
                for (const auto& kv : items->fields) {
                    if (kv.first.empty()) continue;
                    if (kv.second.is_object()) {
                        if (!state_enabled(kv.second)) {
                            ++stats.skipped;
                            continue;
                        }
                        const std::string zh = kv.second.get_string("zh");
                        if (zh.empty()) {
                            ++stats.skipped;
                            continue;
                        }
                        json::set(exact, kv.first, json::make_string(zh));
                        ++stats.exact;
                    } else {
                        const std::string zh = kv.second.as_string();
                        if (zh.empty()) {
                            ++stats.skipped;
                            continue;
                        }
                        json::set(exact, kv.first, json::make_string(zh));
                        ++stats.exact;
                    }
                }
            } else if (items->is_array()) {
                for (const auto& it : items->items) {
                    const std::string src = it.get_string("src");
                    if (src.empty() || !state_enabled(it)) {
                        ++stats.skipped;
                        continue;
                    }
                    const std::string zh = it.get_string("zh");
                    if (zh.empty()) {
                        ++stats.skipped;
                        continue;
                    }
                    json::set(exact, src, json::make_string(zh));
                    ++stats.exact;
                }
            }
        }
        if (const json::Value* pr = doc.find("prefix"); pr && pr->is_array()) {
            for (const auto& it : pr->items) {
                const std::string from = it.get_string("from");
                const std::string to = it.get_string("to");
                if (from.empty() || to.empty() || !state_enabled(it)) {
                    ++stats.skipped;
                    continue;
                }
                json::Value o = json::make_object();
                json::set(o, "from", json::make_string(from));
                json::set(o, "to", json::make_string(to));
                json::push(prefix, o);
                ++stats.prefix;
            }
        }
        if (const json::Value* il = doc.find("inline"); il && il->is_array()) {
            for (const auto& it : il->items) {
                const std::string from = it.get_string("from");
                const std::string to = it.get_string("to");
                if (from.empty() || to.empty() || !state_enabled(it)) {
                    ++stats.skipped;
                    continue;
                }
                json::Value o = json::make_object();
                json::set(o, "from", json::make_string(from));
                json::set(o, "to", json::make_string(to));
                json::push(inline_rules, o);
                ++stats.inline_rules;
            }
        }
    }
    json::set(dict, "exact", exact);
    json::set(dict, "prefix", prefix);
    json::set(dict, "inline", inline_rules);
    json::set(dict, "tool", json::make_string("SFSHanInjector"));
    return true;
}

}  // namespace catalog
