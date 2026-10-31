// 文本工具：UTF-8 <-> UTF-16，路径、字符串小工具。
// 手写转换，不依赖 locale / 系统代码页，避免中文在 GBK 环境被搞坏。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace text {

// UTF-8 -> UTF-16（非法字节按 U+FFFD 处理）
std::u16string utf8_to_utf16(const std::string& s);
// UTF-16 -> UTF-8
std::string utf16_to_utf8(const std::u16string& s);
// 便捷：UTF-8 转宽字符（Windows API 用）
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

// UTF-16 里是否含非 ASCII
bool has_non_ascii(const std::u16string& s);
// 统计 UTF-16 码元数（= 该字符串在 #US 里占的字节数 / 2）
size_t units(const std::u16string& s);

std::string trim(const std::string& s);
std::string lower_ascii(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, const std::string& sep);

}  // namespace text
