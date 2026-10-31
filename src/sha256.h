// 自带的 SHA-256（不依赖 bcrypt/openssl，避免链接依赖）。
#pragma once

#include <cstdint>
#include <string>

namespace sha256util {

// 返回 64 位小写十六进制
std::string hex_digest(const void* data, size_t size);
inline std::string hex_digest(const std::string& s) { return hex_digest(s.data(), s.size()); }

}  // namespace sha256util
