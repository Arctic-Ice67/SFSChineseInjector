#include "fileio.h"

#include <algorithm>

#include <windows.h>

#include "text.h"

namespace fileio {
namespace {

std::wstring w(const std::string& s) { return text::utf8_to_wide(s); }

std::string last_error(const std::string& prefix) {
    const DWORD code = GetLastError();
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (Win32 错误 %lu)", static_cast<unsigned long>(code));
    return prefix + buf;
}

}  // namespace

bool read_all(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    HANDLE h = CreateFileW(w(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = last_error("打不开文件：" + path);
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        err = last_error("读取文件大小失败：" + path);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 30));
        if (!ReadFile(h, out.data() + done, want, &read, nullptr)) {
            CloseHandle(h);
            err = last_error("读取文件失败：" + path);
            return false;
        }
        if (read == 0) break;
        done += read;
    }
    out.resize(done);
    CloseHandle(h);
    return true;
}

bool write_all(const std::string& path, const void* data, size_t size, std::string& err) {
    HANDLE h = CreateFileW(w(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = last_error("无法写入：" + path);
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 30));
        if (!WriteFile(h, p + done, chunk, &wrote, nullptr)) {
            CloseHandle(h);
            err = last_error("写入失败：" + path);
            return false;
        }
        done += wrote;
    }
    CloseHandle(h);
    return true;
}

bool exists(const std::string& path) {
    const DWORD attr = GetFileAttributesW(w(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool is_dir(const std::string& path) {
    const DWORD attr = GetFileAttributesW(w(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool file_size(const std::string& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(w(path).c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return true;
}

bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    if (is_dir(path)) return true;
    // 逐级创建
    std::string acc;
    std::vector<std::string> parts;
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        const bool sep = (c == '\\' || c == '/');
        if (sep) {
            if (!acc.empty()) parts.push_back(acc);
            acc.clear();
            if (i == 2 && path.size() > 2 && path[1] == ':') {
                // 盘符根，如 D:\
                acc = path.substr(0, 3);
            }
            continue;
        }
        acc.push_back(c);
    }
    if (!acc.empty()) parts.push_back(acc);

    std::string cur = path.size() > 2 && path[1] == ':' ? path.substr(0, 3) : std::string();
    if (!cur.empty() && cur.back() != '\\') cur.push_back('\\');
    for (size_t i = 0; i < parts.size(); ++i) {
        // 盘符部分已经放进 cur
        if (i == 0 && parts[i].size() == 2 && parts[i][1] == ':') {
            cur = parts[i] + "\\";
            continue;
        }
        cur += parts[i];
        if ((cur.size() > 3 && cur.back() != '\\' && cur.back() != '/')) {
            if (!is_dir(cur)) {
                if (!CreateDirectoryW(w(cur).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
        }
        if (i + 1 < parts.size() && cur.back() != '\\') cur.push_back('\\');
    }
    return is_dir(path);
}

bool copy_file(const std::string& from, const std::string& to, bool overwrite, std::string& err) {
    if (!overwrite && exists(to)) {
        err = "目标已存在：" + to;
        return false;
    }
    if (!CopyFileW(w(from).c_str(), w(to).c_str(), overwrite ? FALSE : TRUE)) {
        err = last_error("复制失败：" + from + " -> " + to);
        return false;
    }
    return true;
}

bool remove_file(const std::string& path, std::string& err) {
    if (!exists(path)) return true;
    if (!DeleteFileW(w(path).c_str())) {
        err = last_error("删除失败：" + path);
        return false;
    }
    return true;
}

bool rename_file(const std::string& from, const std::string& to, std::string& err) {
    if (!MoveFileExW(w(from).c_str(), w(to).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = last_error("改名失败：" + from + " -> " + to);
        return false;
    }
    return true;
}

bool list_dir(const std::string& path, std::vector<DirEntry>& out, std::string& err) {
    out.clear();
    WIN32_FIND_DATAW fd{};
    const std::string pattern = join_path(path, "*");
    HANDLE h = FindFirstFileW(w(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        err = last_error("列目录失败：" + path);
        return false;
    }
    do {
        std::string name = text::wide_to_utf8(fd.cFileName);
        if (name == "." || name == "..") continue;
        DirEntry e;
        e.name = name;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string file_name(const std::string& path) {
    const size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string file_stem(const std::string& path) {
    const std::string n = file_name(path);
    const size_t p = n.find_last_of('.');
    return p == std::string::npos ? n : n.substr(0, p);
}

std::string file_ext(const std::string& path) {
    const std::string n = file_name(path);
    const size_t p = n.find_last_of('.');
    if (p == std::string::npos) return std::string();
    std::string e = n.substr(p);
    for (char& c : e) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return e;
}

std::string parent_dir(const std::string& path) {
    const size_t p = path.find_last_of("\\/");
    if (p == std::string::npos) return std::string();
    if (p == 0) return path.substr(0, 1);
    return path.substr(0, p);
}

std::string absolute(const std::string& path) {
    DWORD need = GetFullPathNameW(w(path).c_str(), 0, nullptr, nullptr);
    if (need == 0) return path;
    std::wstring buf(need, L'\0');
    const DWORD got = GetFullPathNameW(w(path).c_str(), need, buf.data(), nullptr);
    if (got == 0) return path;
    buf.resize(got);
    return text::wide_to_utf8(buf);
}

}  // namespace fileio
