#include "clr_image.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "fileio.h"
#include "text.h"

namespace clr {
namespace {

// ---------------- 小工具 ----------------
uint16_t rd16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) return 0;
    return static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
}
uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    if (o + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}
bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    return fileio::read_all(path, out, err);
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& err) {
    return fileio::write_all(path, data.data(), data.size(), err);
}

// ---------------- metadata 表结构 ----------------
enum ColKind : uint8_t { K_FIXED = 0, K_STR, K_GUID, K_BLOB, K_TABLE, K_CODED };

enum CodedId : uint8_t {
    C_TypeDefOrRef = 0,
    C_HasConstant,
    C_HasCustomAttribute,
    C_HasFieldMarshal,
    C_HasDeclSecurity,
    C_MemberRefParent,
    C_HasSemantics,
    C_MethodDefOrRef,
    C_MemberForwarded,
    C_Implementation,
    C_CustomAttributeType,
    C_ResolutionScope,
    C_TypeOrMethodDef,
    C_CodedCount
};

struct CodedInfo {
    uint8_t bits;
    std::vector<uint8_t> tables;
};

const std::array<CodedInfo, C_CodedCount>& coded_table() {
    static const std::array<CodedInfo, C_CodedCount> t = {{
        {2, {0x02, 0x01, 0x1B}},
        {2, {0x04, 0x08, 0x17}},
        {5, {0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14, 0x11,
             0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B}},
        {1, {0x04, 0x08}},
        {2, {0x02, 0x06, 0x20}},
        {3, {0x02, 0x01, 0x1A, 0x06, 0x1B}},
        {1, {0x14, 0x17}},
        {1, {0x06, 0x0A}},
        {1, {0x04, 0x06}},
        {2, {0x26, 0x23, 0x27}},
        {3, {0x06, 0x0A}},
        {2, {0x00, 0x1A, 0x23, 0x01}},
        {1, {0x02, 0x06}},
    }};
    return t;
}

struct Col {
    uint8_t kind = K_FIXED;
    uint8_t size = 0;   // K_FIXED / K_TABLE(表号) / K_CODED(编码索引)
};

constexpr int TABLES = 0x2D;

const std::array<std::vector<Col>, TABLES>& schemas() {
    static const auto s = [] {
        std::array<std::vector<Col>, TABLES> t{};
        auto F = [](uint8_t n) { return Col{K_FIXED, n}; };
        const Col S{K_STR, 0}, G{K_GUID, 0}, B{K_BLOB, 0};
        auto T = [](uint8_t ti) { return Col{K_TABLE, ti}; };
        auto C = [](uint8_t ci) { return Col{K_CODED, ci}; };

        t[0x00] = {F(2), S, G, G, G};
        t[0x01] = {C(C_ResolutionScope), S, S};
        t[0x02] = {F(4), S, S, C(C_TypeDefOrRef), T(0x04), T(0x06)};
        t[0x03] = {T(0x04)};
        t[0x04] = {F(2), S, B};
        t[0x05] = {T(0x06)};
        t[0x06] = {F(4), F(2), F(2), S, B, T(0x08)};
        t[0x07] = {T(0x08)};
        t[0x08] = {F(2), F(2), S};
        t[0x09] = {T(0x02), C(C_TypeDefOrRef)};
        t[0x0A] = {C(C_MemberRefParent), S, B};
        t[0x0B] = {F(1), F(1), C(C_HasConstant), B};
        t[0x0C] = {C(C_HasCustomAttribute), C(C_CustomAttributeType), B};
        t[0x0D] = {C(C_HasFieldMarshal), B};
        t[0x0E] = {F(2), C(C_HasDeclSecurity), B};
        t[0x0F] = {F(2), F(4), T(0x02)};
        t[0x10] = {F(4), T(0x04)};
        t[0x11] = {B};
        t[0x12] = {T(0x02), C(C_TypeDefOrRef)};
        t[0x13] = {T(0x14)};
        t[0x14] = {F(2), S, C(C_TypeDefOrRef)};
        t[0x15] = {T(0x02), T(0x17)};
        t[0x16] = {T(0x17)};
        t[0x17] = {F(2), S, B};
        t[0x18] = {F(2), T(0x06), C(C_HasSemantics)};
        t[0x19] = {T(0x02), C(C_MethodDefOrRef), C(C_MethodDefOrRef)};
        t[0x1A] = {S};
        t[0x1B] = {B};
        t[0x1C] = {F(2), C(C_MemberForwarded), S, T(0x1A)};
        t[0x1D] = {F(4), T(0x04)};
        t[0x1E] = {F(4), F(4)};
        t[0x1F] = {F(4)};
        t[0x20] = {F(4), F(2), F(2), F(2), F(2), F(4), B, S, S};
        t[0x21] = {F(4)};
        t[0x22] = {F(4), F(4), F(4)};
        t[0x23] = {F(2), F(2), F(2), F(2), F(4), B, S, S, B};
        t[0x24] = {F(4), T(0x23)};
        t[0x25] = {F(4), F(4), F(4)};
        t[0x26] = {F(4), S, B};
        t[0x27] = {F(4), F(4), S, S, C(C_Implementation)};
        t[0x28] = {F(4), F(4), S, C(C_Implementation)};
        t[0x29] = {T(0x02), T(0x02)};
        t[0x2A] = {F(2), F(2), C(C_TypeOrMethodDef), S};
        t[0x2B] = {C(C_MethodDefOrRef), B};
        t[0x2C] = {T(0x2A), C(C_TypeDefOrRef)};
        return t;
    }();
    return s;
}

// 压缩无符号整数
bool read_compressed(const std::vector<uint8_t>& b, size_t off, size_t limit, uint32_t& value, size_t& len) {
    if (off >= limit || off >= b.size()) return false;
    const uint8_t x = b[off];
    if ((x & 0x80) == 0) {
        value = x;
        len = 1;
        return true;
    }
    if ((x & 0xC0) == 0x80) {
        if (off + 2 > limit) return false;
        value = (static_cast<uint32_t>(x & 0x3F) << 8) | b[off + 1];
        len = 2;
        return true;
    }
    if (off + 4 > limit) return false;
    value = (static_cast<uint32_t>(x & 0x1F) << 24) | (static_cast<uint32_t>(b[off + 1]) << 16) |
            (static_cast<uint32_t>(b[off + 2]) << 8) | b[off + 3];
    len = 4;
    return true;
}

std::string read_cstr(const std::vector<uint8_t>& b, size_t off, size_t limit) {
    std::string out;
    while (off < limit && off < b.size() && b[off] != 0) {
        out.push_back(static_cast<char>(b[off]));
        ++off;
    }
    return out;
}

}  // namespace

// ---------------- Image ----------------

bool Image::load(const std::string& path, std::string& err) {
    path_ = path;
    if (!read_file(path, data_, err)) return false;
    original_ = data_;
    if (!parse(err)) return false;
    snapshot_.clear();
    snapshot_.reserve(strings_.size());
    for (const auto& s : strings_) {
        Snapshot snap;
        snap.heap_off = s.heap_off;
        snap.nbytes = s.nbytes;
        snap.content_at = s.content_at;
        snap.site_count = s.sites.size();
        snapshot_.push_back(snap);
    }
    return true;
}

uint32_t Image::rva_to_off(uint32_t rva, bool* ok) const {
    for (const auto& s : sections_) {
        const uint32_t span = std::max(s.vsize, s.rsize);
        if (rva >= s.va && rva < s.va + span) {
            const uint32_t delta = rva - s.va;
            if (delta >= s.rsize && s.rsize != 0) {
                if (ok) *ok = false;
                return 0;
            }
            if (ok) *ok = true;
            return s.raw + delta;
        }
    }
    if (ok) *ok = false;
    return 0;
}

bool Image::parse(std::string& err) {
    if (data_.size() < 0x40) {
        err = "文件太小，不是 PE";
        return false;
    }
    if (data_[0] != 'M' || data_[1] != 'Z') {
        err = "缺少 MZ 头";
        return false;
    }
    const size_t pe = rd32(data_, 0x3C);
    if (pe + 24 > data_.size() || std::memcmp(data_.data() + pe, "PE\0\0", 4) != 0) {
        err = "PE 头无效";
        return false;
    }
    const uint16_t nsec = rd16(data_, pe + 6);
    const uint16_t optsz = rd16(data_, pe + 20);
    const size_t opt = pe + 24;
    if (opt + 2 > data_.size()) {
        err = "可选头截断";
        return false;
    }
    const uint16_t magic = rd16(data_, opt);
    if (magic != 0x10B && magic != 0x20B) {
        err = "未知的可选头格式";
        return false;
    }
    const size_t dd = opt + (magic == 0x10B ? 96 : 112);
    if (dd + 15 * 8 > data_.size()) {
        err = "数据目录截断";
        return false;
    }
    const uint32_t cli_rva = rd32(data_, dd + 14 * 8);
    if (cli_rva == 0) {
        err = "不是 .NET 程序集（没有 CLI 头）";
        return false;
    }
    sections_.clear();
    const size_t sh = opt + optsz;
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t o = sh + 40ull * i;
        if (o + 40 > data_.size()) break;
        Section s;
        char name[9] = {0};
        std::memcpy(name, data_.data() + o, 8);
        s.name = name;
        s.vsize = rd32(data_, o + 8);
        s.va = rd32(data_, o + 12);
        s.rsize = rd32(data_, o + 16);
        s.raw = rd32(data_, o + 20);
        sections_.push_back(s);
    }

    bool ok = false;
    const uint32_t cli_off = rva_to_off(cli_rva, &ok);
    if (!ok || cli_off + 16 > data_.size()) {
        err = "CLI 头位置无效";
        return false;
    }
    const uint32_t md_rva = rd32(data_, cli_off + 8);
    md_off_ = rva_to_off(md_rva, &ok);
    if (!ok || md_off_ + 20 > data_.size()) {
        err = "metadata 根位置无效";
        return false;
    }
    if (std::memcmp(data_.data() + md_off_, "BSJB", 4) != 0) {
        err = "metadata 签名不是 BSJB";
        return false;
    }
    const uint32_t verlen = rd32(data_, md_off_ + 12);
    size_t p = md_off_ + 16ull + verlen;
    if (p + 4 > data_.size()) {
        err = "metadata 版本串越界";
        return false;
    }
    p += 2;  // flags
    const uint16_t nstreams = rd16(data_, p);
    p += 2;
    bool have_us = false, have_tables = false;
    for (uint16_t i = 0; i < nstreams; ++i) {
        if (p + 8 > data_.size()) {
            err = "流目录截断";
            return false;
        }
        const uint32_t off = rd32(data_, p);
        const uint32_t size = rd32(data_, p + 4);
        p += 8;
        const size_t name_end = (p < data_.size()) ? data_.size() : p;
        const std::string name = read_cstr(data_, p, name_end);
        p = (p + name.size() + 1 + 3) & ~size_t(3);
        const uint32_t abs = md_off_ + off;
        if (name == "#US") {
            us_off_ = abs;
            us_size_ = size;
            have_us = true;
        } else if (name == "#~" || name == "#-") {
            tables_off_ = abs;
            tables_size_ = size;
            have_tables = true;
        } else if (name == "#Strings") {
            strings_off_ = abs;
            strings_size_ = size;
        }
    }
    if (!have_us || !have_tables) {
        err = "缺少 #US 或 #~ 流";
        return false;
    }
    if (us_off_ + us_size_ > data_.size() || tables_off_ + tables_size_ > data_.size()) {
        err = "流位置越界";
        return false;
    }

    read_strings_heap();
    if (!read_tables(err)) return false;
    if (!read_user_strings(err)) return false;
    scan_ldstr();
    read_assembly_name();
    return true;
}

bool Image::read_strings_heap() {
    identifiers_.clear();
    if (strings_off_ == 0 || strings_size_ == 0) return true;
    size_t o = strings_off_;
    const size_t end = std::min<size_t>(strings_off_ + strings_size_, data_.size());
    while (o < end) {
        std::string name = read_cstr(data_, o, end);
        if (!name.empty()) identifiers_.push_back(std::move(name));
        o += name.size() + 1;
    }
    return true;
}

bool Image::read_tables(std::string& err) {
    method_rvas_.clear();
    method_names_.clear();
    if (tables_off_ + 24 > data_.size()) {
        err = "#~ 头越界";
        return false;
    }
    const uint8_t heap_sizes = data_[tables_off_ + 6];
    const uint64_t valid = static_cast<uint64_t>(rd32(data_, tables_off_ + 8)) |
                           (static_cast<uint64_t>(rd32(data_, tables_off_ + 12)) << 32);
    size_t p = tables_off_ + 24;
    std::array<uint32_t, 64> rows{};
    for (int i = 0; i < 64; ++i) {
        if (valid & (1ull << i)) {
            if (p + 4 > data_.size()) {
                err = "表行数越界";
                return false;
            }
            rows[i] = rd32(data_, p);
            p += 4;
        }
    }
    const uint8_t strsz = (heap_sizes & 1) ? 4 : 2;
    const uint8_t guidsz = (heap_sizes & 2) ? 4 : 2;
    const uint8_t blobsz = (heap_sizes & 4) ? 4 : 2;

    auto col_size = [&](const Col& c) -> uint8_t {
        switch (c.kind) {
            case K_FIXED: return c.size;
            case K_STR: return strsz;
            case K_GUID: return guidsz;
            case K_BLOB: return blobsz;
            case K_TABLE: return rows[c.size] >= 0x10000 ? 4 : 2;
            case K_CODED: {
                const CodedInfo& ci = coded_table()[c.size];
                uint32_t biggest = 0;
                for (uint8_t t : ci.tables) biggest = std::max(biggest, rows[t]);
                return biggest >= (1u << (16 - ci.bits)) ? 4 : 2;
            }
            default: return 2;
        }
    };
    auto row_size = [&](int t) -> size_t {
        size_t total = 0;
        for (const Col& c : schemas()[t]) total += col_size(c);
        return total;
    };

    std::array<size_t, TABLES> starts{};
    {
        size_t cursor = p;
        for (int t = 0; t < TABLES; ++t) {
            starts[t] = cursor;
            cursor += static_cast<size_t>(rows[t]) * row_size(t);
            if (cursor > data_.size() + 1) {
                // 结构异常，但继续（后面按行数读取时会有边界检查）
                cursor = std::min(cursor, data_.size());
            }
        }
    }

    // ---- MethodDef ----
    const uint32_t md_rows = rows[0x06];
    const size_t md_row_sz = row_size(0x06);
    if (md_rows > 0 && md_row_sz > 0) {
        if (starts[0x06] + static_cast<size_t>(md_rows) * md_row_sz > data_.size()) {
            err = "MethodDef 表越界";
            return false;
        }
        method_rvas_.reserve(md_rows);
        method_names_.reserve(md_rows);
        // MethodDef: RVA(4) ImplFlags(2) Flags(2) Name(str) Sig(blob) ParamList
        const size_t name_col = 8;
        for (uint32_t i = 0; i < md_rows; ++i) {
            const size_t r = starts[0x06] + static_cast<size_t>(i) * md_row_sz;
            method_rvas_.push_back(rd32(data_, r));
            const uint32_t name_idx = strsz == 4 ? rd32(data_, r + name_col) : rd16(data_, r + name_col);
            std::string name;
            if (strings_off_ && name_idx < strings_size_) {
                name = read_cstr(data_, strings_off_ + name_idx,
                                 std::min<size_t>(strings_off_ + strings_size_, data_.size()));
            }
            method_names_.push_back(std::move(name));
        }
    }

    // ---- TypeDef：给方法加上 Namespace.Type 前缀 ----
    const uint32_t td_rows = rows[0x02];
    const size_t td_row_sz = row_size(0x02);
    if (td_rows > 1 && td_row_sz > 0 && md_rows > 0 &&
        starts[0x02] + static_cast<size_t>(td_rows) * td_row_sz <= data_.size()) {
        // TypeDef: Flags(4) Name(str) Namespace(str) Extends(coded) FieldList MethodList
        size_t name_c = 4;
        size_t ns_c = name_c + strsz;
        size_t code_c = ns_c + strsz;
        size_t ml_c = code_c + col_size(Col{K_CODED, C_TypeDefOrRef}) + col_size(Col{K_TABLE, 0x04});
        const size_t ml_sz = col_size(Col{K_TABLE, 0x06});
        auto read_idx = [&](size_t off, uint8_t sz) -> uint32_t {
            return sz == 4 ? rd32(data_, off) : rd16(data_, off);
        };
        for (uint32_t i = 0; i < td_rows; ++i) {
            const size_t r = starts[0x02] + static_cast<size_t>(i) * td_row_sz;
            const uint32_t name_idx = read_idx(r + name_c, strsz);
            const uint32_t ns_idx = read_idx(r + ns_c, strsz);
            std::string tname, tns;
            const size_t limit = std::min<size_t>(strings_off_ + strings_size_, data_.size());
            if (strings_off_ && name_idx < strings_size_) tname = read_cstr(data_, strings_off_ + name_idx, limit);
            if (strings_off_ && ns_idx < strings_size_) tns = read_cstr(data_, strings_off_ + ns_idx, limit);
            const uint32_t first = read_idx(r + ml_c, ml_sz);
            uint32_t last = md_rows + 1;
            if (i + 1 < td_rows) {
                const size_t rn = starts[0x02] + static_cast<size_t>(i + 1) * td_row_sz;
                last = read_idx(rn + ml_c, ml_sz);
            }
            if (first == 0 || last <= first) continue;
            const std::string prefix = (tns.empty() ? tname : (tns + "." + tname)) + "::";
            for (uint32_t m = first; m < last && m <= md_rows; ++m) {
                const size_t idx = static_cast<size_t>(m - 1);
                if (idx < method_names_.size() && !method_names_[idx].empty()) {
                    method_names_[idx] = prefix + method_names_[idx];
                }
            }
        }
    }
    return true;
}

bool Image::read_user_strings(std::string& err) {
    strings_.clear();
    size_t o = us_off_ + 1;  // 堆首字节是空字符串
    const size_t end = std::min<size_t>(us_off_ + us_size_, data_.size());
    while (o < end) {
        uint32_t n = 0;
        size_t nb = 0;
        if (!read_compressed(data_, o, end, n, nb)) break;
        if (n == 0) {
            o += nb;
            continue;
        }
        const size_t content = o + nb;
        if (content + n > end) break;
        UserString us;
        us.heap_off = static_cast<uint32_t>(o - us_off_);
        us.content_at = static_cast<uint32_t>(content);
        us.nbytes = n - 1;               // 末尾 1 字节是 flag
        us.flag_at = static_cast<uint32_t>(content + us.nbytes);
        us.text.resize(us.nbytes / 2);
        for (size_t k = 0; k < us.text.size(); ++k) {
            const uint8_t lo = data_[content + k * 2];
            const uint8_t hi = data_[content + k * 2 + 1];
            us.text[k] = static_cast<char16_t>(lo | (hi << 8));
        }
        strings_.push_back(std::move(us));
        // 关键：条目之间没有对齐填充，直接接下一个
        o = content + n;
    }
    if (strings_.empty()) {
        err = "#US 堆解析为空";
        return false;
    }
    return true;
}

bool Image::scan_ldstr() {
    ldstr_count_ = 0;
    std::unordered_map<uint32_t, size_t> by_off;
    by_off.reserve(strings_.size() * 2 + 8);
    for (size_t i = 0; i < strings_.size(); ++i) by_off.emplace(strings_[i].heap_off, i);

    std::unordered_set<std::string> ident_set;
    ident_set.reserve(identifiers_.size() * 2 + 8);
    for (const auto& id : identifiers_) ident_set.insert(id);

    const size_t total = data_.size();
    for (size_t mi = 0; mi < method_rvas_.size(); ++mi) {
        const uint32_t rva = method_rvas_[mi];
        if (rva == 0) continue;
        bool ok = false;
        const uint32_t moff = rva_to_off(rva, &ok);
        if (!ok || moff >= total) continue;
        size_t il = 0;
        size_t size = 0;
        const uint8_t h = data_[moff];
        if ((h & 3) == 2) {
            il = moff + 1;
            size = h >> 2;
        } else {
            if (moff + 12 > total) continue;
            const uint16_t flags = rd16(data_, moff);
            size_t hsz = static_cast<size_t>(flags >> 12) * 4;
            if (hsz < 12) hsz = 12;
            size = rd32(data_, moff + 4);
            il = moff + hsz;
        }
        if (il >= total) continue;
        if (il + size > total) size = total - il;
        const size_t end = il + size;
        size_t q = il;
        while (q + 5 <= end) {
            if (data_[q] == 0x72) {  // ldstr
                const uint32_t tok = rd32(data_, q + 1);
                if ((tok >> 24) == 0x70) {
                    const uint32_t heap_off = tok & 0xFFFFFFu;
                    auto it = by_off.find(heap_off);
                    if (it != by_off.end()) {
                        UserString& us = strings_[it->second];
                        us.sites.push_back(static_cast<uint32_t>(q + 1));
                        ++ldstr_count_;
                        if (us.owners.size() < 8 && mi < method_names_.size() && !method_names_[mi].empty()) {
                            bool dup = false;
                            for (const auto& o : us.owners) {
                                if (o == method_names_[mi]) {
                                    dup = true;
                                    break;
                                }
                            }
                            if (!dup) us.owners.push_back(method_names_[mi]);
                        }
                        q += 5;
                        continue;
                    }
                }
            }
            ++q;
        }
    }

    // 标识符重合检测（键名/类型名之类翻了会坏）
    for (auto& us : strings_) {
        if (us.text.empty()) continue;
        if (text::has_non_ascii(us.text)) continue;
        const std::string asc = text::utf16_to_utf8(us.text);
        if (ident_set.count(asc)) us.in_strings_heap = true;
    }
    return true;
}

bool Image::read_assembly_name() {
    assembly_name_.clear();
    // Assembly 表 0x20：HashAlgId(4) Major(2) Minor(2) Build(2) Rev(2) Flags(4) PublicKey(blob) Name(str) Culture(str)
    // 直接扫 #Strings 找明显的程序集名不可靠，这里用表结构读。
    // 为了简单，退化为：取 Assembly 表行的 Name 列。
    if (tables_off_ + 24 > data_.size()) return false;
    const uint8_t heap_sizes = data_[tables_off_ + 6];
    const uint64_t valid = static_cast<uint64_t>(rd32(data_, tables_off_ + 8)) |
                           (static_cast<uint64_t>(rd32(data_, tables_off_ + 12)) << 32);
    if (!(valid & (1ull << 0x20))) return false;
    size_t p = tables_off_ + 24;
    std::array<uint32_t, 64> rows{};
    for (int i = 0; i < 64; ++i) {
        if (valid & (1ull << i)) {
            rows[i] = rd32(data_, p);
            p += 4;
        }
    }
    if (rows[0x20] == 0) return false;
    const uint8_t strsz = (heap_sizes & 1) ? 4 : 2;
    const uint8_t blobsz = (heap_sizes & 4) ? 4 : 2;
    auto row_size_of = [&](int t) -> size_t {
        size_t total = 0;
        for (const Col& c : schemas()[t]) {
            switch (c.kind) {
                case K_FIXED: total += c.size; break;
                case K_STR: total += strsz; break;
                case K_GUID: total += (heap_sizes & 2) ? 4 : 2; break;
                case K_BLOB: total += blobsz; break;
                case K_TABLE: total += rows[c.size] >= 0x10000 ? 4 : 2; break;
                case K_CODED: {
                    const CodedInfo& ci = coded_table()[c.size];
                    uint32_t biggest = 0;
                    for (uint8_t t2 : ci.tables) biggest = std::max(biggest, rows[t2]);
                    total += biggest >= (1u << (16 - ci.bits)) ? 4 : 2;
                    break;
                }
            }
        }
        return total;
    };
    size_t start = p;
    for (int t = 0; t < 0x20; ++t) start += static_cast<size_t>(rows[t]) * row_size_of(t);
    // Name 列：4+2+2+2+2+4 + blob + 0 = 16 + blobsz
    const size_t name_off = start + 16 + blobsz;
    if (name_off + strsz > data_.size()) return false;
    const uint32_t idx = strsz == 4 ? rd32(data_, name_off) : rd16(data_, name_off);
    if (!strings_off_ || idx >= strings_size_) return false;
    assembly_name_ = read_cstr(data_, strings_off_ + idx,
                               std::min<size_t>(strings_off_ + strings_size_, data_.size()));
    return true;
}

bool Image::replace(size_t index, const std::u16string& zh, std::string& err) {
    if (index >= strings_.size()) {
        err = "索引越界";
        return false;
    }
    UserString& us = strings_[index];
    const size_t want = zh.size() * 2;
    if (want > us.nbytes) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "译文太长：需要 %zu 字节，原文只有 %u 字节（%s）", want,
                      us.nbytes, text::utf16_to_utf8(us.text).c_str());
        err = buf;
        return false;
    }
    std::u16string filled = zh;
    while (filled.size() * 2 < us.nbytes) filled.push_back(static_cast<char16_t>(0x200B));  // 零宽空格
    if (filled.size() * 2 != us.nbytes) {
        err = "内部错误：补齐后长度不匹配（奇数字节）";
        return false;
    }
    if (us.content_at + us.nbytes > data_.size()) {
        err = "写入区间越界";
        return false;
    }
    std::memcpy(data_.data() + us.content_at, filled.data(), us.nbytes);
    // 正确维护 #US 的「含非 ASCII」标志字节
    if (us.flag_at < data_.size()) {
        data_[us.flag_at] = text::has_non_ascii(filled) ? 1 : 0;
    }
    us.text = std::move(filled);
    return true;
}

bool Image::save_as(const std::string& path, std::string& err) const {
    return write_file(path, data_, err);
}

bool Image::diff_ranges(std::vector<std::pair<uint32_t, uint32_t>>& ranges) const {
    ranges.clear();
    if (original_.size() != data_.size()) return false;
    size_t i = 0;
    const size_t n = data_.size();
    while (i < n) {
        if (data_[i] == original_[i]) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < n && data_[i] != original_[i]) ++i;
        ranges.emplace_back(static_cast<uint32_t>(start), static_cast<uint32_t>(i));
    }
    return true;
}

bool Image::self_check(std::string& err) const {
    if (original_.size() != data_.size()) {
        err = "文件大小变了（原地补丁不应该改变大小）";
        return false;
    }
    // 1) 所有改动必须落在某条字面量的内容区间内
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (!diff_ranges(ranges)) {
        err = "无法比对改动区间";
        return false;
    }
    for (const auto& r : ranges) {
        bool inside = false;
        for (const auto& s : strings_) {
            // 内容区与末尾 flag 字节是连续的，一起算
            if (r.first >= s.content_at && r.second <= static_cast<uint64_t>(s.flag_at) + 1) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "改动越界：0x%X..0x%X 不在任何 #US 内容区内", r.first, r.second);
            err = buf;
            return false;
        }
    }
    // 2) 重新解析，确认条目数/偏移/长度/引用点与装载时一致
    Image fresh;
    fresh.path_ = path_;
    fresh.data_ = data_;
    fresh.original_ = data_;
    std::string perr;
    if (!fresh.parse(perr)) {
        err = "补丁后重新解析失败：" + perr;
        return false;
    }
    if (fresh.strings_.size() != snapshot_.size()) {
        err = "补丁后 #US 条目数变了";
        return false;
    }
    size_t mismatched = 0;
    for (size_t i = 0; i < snapshot_.size(); ++i) {
        const auto& a = snapshot_[i];
        const auto& b = fresh.strings_[i];
        if (a.heap_off != b.heap_off || a.nbytes != b.nbytes || a.content_at != b.content_at ||
            a.site_count != b.sites.size()) {
            ++mismatched;
        }
    }
    if (mismatched) {
        err = "补丁后有 " + std::to_string(mismatched) + " 条字面量的偏移/长度/引用发生了变化";
        return false;
    }
    if (fresh.method_count() != method_rvas_.size()) {
        err = "补丁后方法数变了";
        return false;
    }
    return true;
}

}  // namespace clr
