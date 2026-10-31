// 字体体检：解析 TTF/OTF/TTC 的 cmap，统计对「汉化包实际用字」的覆盖。
// 目的：玩家导入字体时先检查，缺字太多就拦下来，避免满屏方块。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fontcheck {

struct Result {
    size_t glyphs = 0;              // maxp 里的字形数
    size_t codepoints = 0;          // cmap 覆盖的码位数
    std::vector<char32_t> missing;  // 需要但字体里没有的字符
};

// 读取 path 的 cmap，检查它是否覆盖 needed 里的每个字符
bool check(const std::string& path, const std::u16string& needed, Result& out, std::string& err);

// 只做「是不是能当字体用」的粗检
bool looks_like_font(const std::string& path);

}  // namespace fontcheck
