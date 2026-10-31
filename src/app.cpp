#include "app.h"

#include <windows.h>

#include "fileio.h"
#include "text.h"

namespace app {

std::string exe_dir() {
    wchar_t buf[2048];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 2048);
    if (n == 0) return std::string();
    const std::string full = text::wide_to_utf8(std::wstring(buf, n));
    return fileio::parent_dir(full);
}

std::string data_dir() {
    // 正式版：data 与 exe 同级；开发时：exe 在 build\，data 在工程根
    const std::string local = fileio::join_path(exe_dir(), "data");
    if (fileio::is_dir(local)) return local;
    const std::string up = fileio::join_path(fileio::parent_dir(exe_dir()), "data");
    if (fileio::is_dir(up)) return up;
    return local;
}

std::string data_file(const std::string& rel) { return fileio::join_path(data_dir(), rel); }

std::string catalog_dir() { return data_file("catalog"); }
std::string translations_dir() { return data_file("translations"); }
std::string fonts_dir() { return data_file("fonts"); }
std::string runtime_dir() { return data_file("runtime"); }

}  // namespace app
