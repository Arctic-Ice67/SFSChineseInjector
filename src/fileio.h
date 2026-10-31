// 文件 IO：统一走 Win32 API（路径用 UTF-8 传入、内部转 UTF-16），
// 避免 MinGW libstdc++ 的窄字符路径把中文目录搞坏。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fileio {

bool read_all(const std::string& path, std::vector<uint8_t>& out, std::string& err);
bool write_all(const std::string& path, const void* data, size_t size, std::string& err);
inline bool write_all(const std::string& path, const std::string& s, std::string& err) {
    return write_all(path, s.data(), s.size(), err);
}
bool exists(const std::string& path);
bool is_dir(const std::string& path);
bool file_size(const std::string& path, uint64_t& size);
bool ensure_dir(const std::string& path);
bool copy_file(const std::string& from, const std::string& to, bool overwrite, std::string& err);
bool remove_file(const std::string& path, std::string& err);
bool rename_file(const std::string& from, const std::string& to, std::string& err);

// 列出目录下的文件/子目录（name 是纯名字，不含路径）
struct DirEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};
bool list_dir(const std::string& path, std::vector<DirEntry>& out, std::string& err);

// 路径拼接（自动补分隔符）
std::string join_path(const std::string& a, const std::string& b);
std::string file_name(const std::string& path);        // 不含目录
std::string file_stem(const std::string& path);        // 去扩展名
std::string file_ext(const std::string& path);         // 含点，小写
std::string parent_dir(const std::string& path);
std::string absolute(const std::string& path);

}  // namespace fileio
