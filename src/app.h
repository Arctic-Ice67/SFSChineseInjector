// 程序自身的位置与常量（素材都放在 exe 同级的 data\ 下）。
#pragma once

#include <string>

namespace app {

constexpr const char* kName = "SFSHanInjector";
constexpr const char* kVersion = "0.1.0";
constexpr const char* kTitle = "SFS 汉化注入器";

std::string exe_dir();               // 不含尾部反斜杠
std::string data_dir();              // exe_dir\data
std::string data_file(const std::string& rel);  // data_dir\<rel>
std::string catalog_dir();
std::string translations_dir();
std::string fonts_dir();
std::string runtime_dir();

}  // namespace app
