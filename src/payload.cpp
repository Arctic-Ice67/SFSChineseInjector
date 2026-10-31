// 单文件载荷：构建时把资源打成自定义容器追加到 exe 末尾。
// 布局：[exe][payload][u32 payload_offset][8B "SFSHANEND"]
// payload：[u32 magic 'SFSH'][u32 count][entry...]
// entry  ：[u16 nameLen][u32 dataLen][name][data]
// 不压缩、不校验 CRC —— 只有我们自己读，没必要。
#include "payload.h"

#include <windows.h>

#include <cstring>

#include "app.h"
#include "fileio.h"

namespace payload {
namespace {

constexpr uint32_t kMagic = 0x48534653;   // 'SFSH'
constexpr char kEnd[8] = {'S', 'F', 'S', 'H', 'A', 'N', 'E', 'N'};

struct Entry {
    std::string name;
    size_t data_off;
    uint32_t size;
};

std::vector<uint8_t> read_self() {
    std::vector<uint8_t> out;
    wchar_t buf[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    HANDLE f = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (256LL << 20)) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) ||
            got != out.size()) {
            out.clear();
        }
    }
    CloseHandle(f);
    return out;
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool locate(const std::vector<uint8_t>& self, size_t& payload_start) {
    if (self.size() < 12) return false;
    if (std::memcmp(&self[self.size() - 8], kEnd, 8) != 0) return false;
    const uint32_t start = rd32(&self[self.size() - 12]);
    if (start >= self.size() - 12) return false;
    if (rd32(&self[start]) != kMagic) return false;
    payload_start = start;
    return true;
}

bool parse(const std::vector<uint8_t>& self, std::vector<Entry>& entries, size_t& payload_start) {
    if (!locate(self, payload_start)) return false;
    size_t p = payload_start + 4;
    if (p + 4 > self.size()) return false;
    const uint32_t count = rd32(&self[p]);
    p += 4;
    if (count > 4096) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 6 > self.size()) return false;
        const uint16_t nlen = rd16(&self[p]);
        const uint32_t dlen = rd32(&self[p + 2]);
        p += 6;
        if (p + nlen + static_cast<size_t>(dlen) > self.size()) return false;
        Entry e{};
        e.name.assign(reinterpret_cast<const char*>(&self[p]), nlen);
        p += nlen;
        e.data_off = p;
        e.size = dlen;
        entries.push_back(e);
        p += dlen;
    }
    return true;
}

std::string g_temp_file;

}  // namespace

bool present() {
    std::vector<uint8_t> self = read_self();
    std::vector<Entry> entries;
    size_t start = 0;
    return parse(self, entries, start) && !entries.empty();
}

bool get(const std::string& name, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<uint8_t> self = read_self();
    std::vector<Entry> entries;
    size_t start = 0;
    if (!parse(self, entries, start)) return false;
    for (const auto& e : entries) {
        if (e.name != name) continue;
        out.assign(self.begin() + e.data_off, self.begin() + e.data_off + e.size);
        return true;
    }
    return false;
}

std::string debug_info() {
    std::vector<uint8_t> self = read_self();
    std::string s = "self_size=" + std::to_string(self.size());
    std::vector<Entry> entries;
    size_t start = 0;
    if (!parse(self, entries, start)) return s + " no_payload";
    s += " payload_at=" + std::to_string(start) + " entries=" + std::to_string(entries.size());
    for (const auto& e : entries) s += " | " + e.name + " " + std::to_string(e.size);
    return s;
}

std::vector<std::string> list() {
    std::vector<std::string> names;
    std::vector<uint8_t> self = read_self();
    std::vector<Entry> entries;
    size_t start = 0;
    if (!parse(self, entries, start)) return names;
    for (const auto& e : entries) names.push_back(e.name);
    return names;
}

bool asset(const std::string& name, std::vector<uint8_t>& out) {
    if (get(name, out)) return true;
    std::string err;
    return fileio::read_all(app::data_file(name), out, err);
}

std::string asset_to_temp(const std::string& name, const std::string& suffix) {
    std::vector<uint8_t> bytes;
    if (!asset(name, bytes)) return std::string();
    char tmp[MAX_PATH] = {0};
    ::GetTempPathA(MAX_PATH, tmp);
    const std::string path = std::string(tmp) + "sfshan_" +
                             std::to_string(::GetTickCount64() % 1000000) + suffix;
    std::string err;
    if (!fileio::write_all(path, bytes.data(), bytes.size(), err)) return std::string();
    g_temp_file = path;
    return path;
}

void cleanup_temp() {
    if (g_temp_file.empty()) return;
    std::string err;
    fileio::remove_file(g_temp_file, err);
    g_temp_file.clear();
}

}  // namespace payload
