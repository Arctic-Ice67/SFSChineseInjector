// 单文件载荷：构建时把 data 目录打包成 zip 追加到 exe 末尾，
// 运行时读自己、从追加的 zip 里取文件（只支持 store 存储，不压缩，免去 zlib）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace payload {

// exe 末尾是否带载荷
bool present();

// 从载荷取文件（名字用 '/' 分隔，例如 "runtime/SFSHanRuntime.dll"）
bool get(const std::string& name, std::vector<uint8_t>& out);

std::string debug_info();
// 列出载荷里的所有文件名（调试用）
std::vector<std::string> list();

// 统一资源入口：优先载荷，其次磁盘 data 目录
bool asset(const std::string& name, std::vector<uint8_t>& out);

// 把资源落到临时目录（字体这种只能走路径的用），返回路径；失败返回空
std::string asset_to_temp(const std::string& name, const std::string& suffix);

// 清掉用过的临时文件
void cleanup_temp();

}  // namespace payload
