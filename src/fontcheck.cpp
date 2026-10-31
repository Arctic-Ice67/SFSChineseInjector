#include "fontcheck.h"

#include <cstring>
#include <fstream>
#include <unordered_set>

#include "fileio.h"

namespace fontcheck {
namespace {

uint16_t rd16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) return 0;
    return static_cast<uint16_t>(b[o] << 8 | b[o + 1]);
}
uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    if (o + 4 > b.size()) return 0;
    return (static_cast<uint32_t>(b[o]) << 24) | (static_cast<uint32_t>(b[o + 1]) << 16) |
           (static_cast<uint32_t>(b[o + 2]) << 8) | b[o + 3];
}

// 找到 sfnt 的起点（TTC 取第一个字体）
size_t sfnt_offset(const std::vector<uint8_t>& b, std::string& err) {
    if (b.size() < 12) {
        err = "文件太小";
        return 0;
    }
    const uint32_t tag = rd32(b, 0);
    if (tag == 0x74746366u) {  // "ttcf"
        if (rd32(b, 8) == 0) {
            err = "TTC 里没有字体";
            return 0;
        }
        return rd32(b, 12);
    }
    return 0;
}

bool collect_cmap(const std::vector<uint8_t>& b, size_t base, std::unordered_set<uint32_t>& out,
                  std::string& err) {
    const uint16_t num_tables = rd16(b, base + 4);
    if (num_tables == 0 || base + 12 + 16ull * num_tables > b.size()) {
        err = "表目录异常";
        return false;
    }
    size_t cmap_off = 0, maxp_off = 0;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const size_t rec = base + 12 + 16ull * i;
        const uint32_t tag = rd32(b, rec);
        const uint32_t off = rd32(b, rec + 8);
        if (tag == 0x636d6170u) cmap_off = off;        // "cmap"
        else if (tag == 0x6d617870u) maxp_off = off;   // "maxp"
    }
    if (!cmap_off || cmap_off + 4 > b.size()) {
        err = "没有 cmap 表";
        return false;
    }
    const uint16_t n = rd16(b, cmap_off + 2);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t rec = cmap_off + 4 + 8ull * i;
        if (rec + 8 > b.size()) break;
        const uint32_t sub = cmap_off + rd32(b, rec + 4);
        if (sub + 2 > b.size()) continue;
        const uint16_t fmt = rd16(b, sub);
        if (fmt == 4) {
            const uint16_t segx2 = rd16(b, sub + 6);
            const uint16_t seg = segx2 / 2;
            const size_t ends = sub + 14;
            const size_t starts = ends + segx2 + 2;
            const size_t deltas = starts + segx2;
            const size_t ranges = deltas + segx2;
            for (uint16_t s = 0; s < seg; ++s) {
                const uint16_t st = rd16(b, starts + 2 * s);
                const uint16_t en = rd16(b, ends + 2 * s);
                if (st == 0xFFFF) continue;
                const uint16_t delta = rd16(b, deltas + 2 * s);
                const uint16_t ro = rd16(b, ranges + 2 * s);
                for (uint32_t c = st; c <= en && c != 0xFFFFu; ++c) {
                    uint16_t g = 0;
                    if (ro == 0) {
                        g = static_cast<uint16_t>(c + delta);
                    } else {
                        const size_t gi = ranges + 2 * s + ro + 2 * (c - st);
                        if (gi + 2 > b.size()) break;
                        g = rd16(b, gi);
                        if (g) g = static_cast<uint16_t>(g + delta);
                    }
                    if (g) out.insert(c);
                }
            }
        } else if (fmt == 12) {
            const uint32_t groups = rd32(b, sub + 12);
            for (uint32_t g = 0; g < groups && g < 200000; ++g) {
                const size_t p = sub + 16 + 12ull * g;
                if (p + 12 > b.size()) break;
                const uint32_t sc = rd32(b, p);
                const uint32_t ec = rd32(b, p + 4);
                for (uint32_t c = sc; c <= ec && c - sc < 65536; ++c) out.insert(c);
            }
        } else if (fmt == 6) {
            const uint16_t first = rd16(b, sub + 6);
            const uint16_t count = rd16(b, sub + 8);
            for (uint16_t k = 0; k < count; ++k) {
                if (rd16(b, sub + 10 + 2 * k)) out.insert(first + k);
            }
        } else if (fmt == 0) {
            for (uint16_t c = 0; c < 256; ++c) {
                if (b.size() > sub + 6 + c && b[sub + 6 + c]) out.insert(c);
            }
        }
    }
    if (out.empty()) {
        err = "cmap 里没有任何字形映射";
        return false;
    }
    return true;
}

}  // namespace

bool looks_like_font(const std::string& path) {
    std::vector<uint8_t> b;
    std::string err;
    if (!fileio::read_all(path, b, err)) return false;
    if (b.size() < 12) return false;
    const uint32_t tag = rd32(b, 0);
    return tag == 0x00010000u || tag == 0x4F54544Fu /*OTTO*/ || tag == 0x74727565u /*true*/ ||
           tag == 0x74746366u /*ttcf*/;
}

bool check(const std::string& path, const std::u16string& needed, Result& out, std::string& err) {
    out = Result{};
    std::vector<uint8_t> b;
    if (!fileio::read_all(path, b, err)) return false;
    const size_t base = sfnt_offset(b, err);
    if (!err.empty()) return false;
    std::unordered_set<uint32_t> cps;
    if (!collect_cmap(b, base, cps, err)) return false;
    out.codepoints = cps.size();

    // maxp 字形数
    const uint16_t num_tables = rd16(b, base + 4);
    for (uint16_t i = 0; i < num_tables; ++i) {
        const size_t rec = base + 12 + 16ull * i;
        if (rd32(b, rec) == 0x6d617870u) {
            out.glyphs = rd16(b, rd32(b, rec + 8) + 4);
            break;
        }
    }

    for (char16_t ch : needed) {
        if (ch < 0x20) continue;
        if (!cps.count(ch)) {
            bool dup = false;
            for (char32_t m : out.missing) {
                if (m == ch) {
                    dup = true;
                    break;
                }
            }
            if (!dup) out.missing.push_back(ch);
        }
    }
    return true;
}

}  // namespace fontcheck
