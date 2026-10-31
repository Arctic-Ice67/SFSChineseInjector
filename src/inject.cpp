// 手动映射器实现。所有操作都用 Win32 API，DLL 只存在于内存里。
#include "inject.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>


namespace inject {
namespace {

constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kPe32Plus = 0x20B;

// ⚠ 必须和 native\bootstrap\bootstrap.c 的 HAN_PARAMS 逐字段一致（顺序/类型/大小）
struct Params {
    wchar_t mono_dll[512];   // 1024 B
    uint64_t managed_bytes;
    int32_t managed_len;
    uint64_t cfg_bytes;
    int32_t cfg_len;
    char asm_name[256];
    char type_ns[256];
    char type_name[256];
    char method_name[128];
    wchar_t log_path[512];
};

void copy_str(char* dst, size_t cap, const char* src) {
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = 0;
}

void copy_wstr(wchar_t* dst, size_t cap, const std::wstring& src) {
    size_t n = src.size();
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src.c_str(), n * sizeof(wchar_t));
    dst[n] = 0;
}

const IMAGE_SECTION_HEADER* section_table(const IMAGE_NT_HEADERS64* nt) {
    return IMAGE_FIRST_SECTION(nt);
}

bool parse(const std::vector<uint8_t>& img, const IMAGE_DOS_HEADER** dos,
           const IMAGE_NT_HEADERS64** nt, std::string& err) {
    if (img.size() < sizeof(IMAGE_DOS_HEADER)) {
        err = "文件太小";
        return false;
    }
    const auto* d = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.data());
    if (d->e_magic != IMAGE_DOS_SIGNATURE) {
        err = "不是 PE 文件";
        return false;
    }
    if (static_cast<size_t>(d->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > img.size()) {
        err = "e_lfanew 越界";
        return false;
    }
    const auto* n = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.data() + d->e_lfanew);
    if (n->Signature != IMAGE_NT_SIGNATURE) {
        err = "NT 头签名不对";
        return false;
    }
    if (n->FileHeader.Machine != kMachineAmd64) {
        err = "不是 x64 DLL";
        return false;
    }
    if (n->OptionalHeader.Magic != kPe32Plus) {
        err = "不是 PE32+";
        return false;
    }
    *dos = d;
    *nt = n;
    return true;
}

}  // namespace

uint32_t find_process(const std::wstring& exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            // 忽略大小写比较
            std::wstring a = name, b = exe_name;
            for (auto& c : a) c = static_cast<wchar_t>(::towlower(c));
            for (auto& c : b) c = static_cast<wchar_t>(::towlower(c));
            if (a == b) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool remote_write(void* hproc, const void* data, size_t len, uint64_t* out_addr, std::string& err) {
    auto hp = static_cast<HANDLE>(hproc);
    void* mem = VirtualAllocEx(hp, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        err = "VirtualAllocEx 失败";
        return false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(hp, mem, data, len, &written) || written != len) {
        err = "WriteProcessMemory 失败";
        return false;
    }
    *out_addr = reinterpret_cast<uint64_t>(mem);
    return true;
}

bool wait_for_module(uint32_t pid, const wchar_t* module_name, uint32_t timeout_ms) {
    const uint32_t step = 500;
    for (uint32_t waited = 0;; waited += step) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            bool found = false;
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, module_name) == 0) {
                        found = true;
                        break;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            if (found) return true;
        }
        if (waited >= timeout_ms) return false;
        ::Sleep(step);
    }
}
bool manual_map(void* hproc, const std::vector<uint8_t>& img, uint64_t* base_out, std::string& err) {
    const IMAGE_DOS_HEADER* dos = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    if (!parse(img, &dos, &nt, err)) return false;
    (void)dos;

    const size_t image_size = nt->OptionalHeader.SizeOfImage;
    const size_t headers_size = nt->OptionalHeader.SizeOfHeaders;
    if (image_size == 0 || headers_size == 0 || headers_size > image_size) {
        err = "镜像大小异常";
        return false;
    }

    auto hp = static_cast<HANDLE>(hproc);
    // 优先申请镜像自己的 PreferredBase：拿得到就不用做重定位
    void* remote = VirtualAllocEx(hp, reinterpret_cast<void*>(nt->OptionalHeader.ImageBase),
                                  image_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) {
        remote = VirtualAllocEx(hp, nullptr, image_size, MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE);
    }
    if (!remote) {
        err = "VirtualAllocEx(镜像) 失败";
        return false;
    }
    const uint64_t remote_base = reinterpret_cast<uint64_t>(remote);

    // 1) 本地拼出完整镜像（头 + 各节），顺便做重定位/导入修复，最后一次写入
    std::vector<uint8_t> local(image_size, 0);
    const size_t copy_headers = headers_size < img.size() ? headers_size : img.size();
    std::memcpy(local.data(), img.data(), copy_headers);

    const IMAGE_SECTION_HEADER* sec = section_table(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sec[i];
        if (s.SizeOfRawData == 0) continue;
        const size_t dst = s.VirtualAddress;
        const size_t src = s.PointerToRawData;
        if (dst >= image_size || src >= img.size()) continue;
        size_t n = s.SizeOfRawData;
        if (dst + n > image_size) n = image_size - dst;
        if (src + n > img.size()) n = img.size() - src;
        std::memcpy(local.data() + dst, img.data() + src, n);
    }

    // 2) 重定位
    const uint64_t delta = remote_base - nt->OptionalHeader.ImageBase;
    if (delta != 0) {
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (dir.Size == 0) {
            // 小 DLL 若是全位置无关（只用 RIP 相对寻址）就可以直接映射；
            // 这里记一条警告继续，真出问题会在远程线程里崩，日志能看到。
            OutputDebugStringA("[inject] 镜像无重定位表，按位置无关处理\n");
        }
        size_t off = 0;
        while (off + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size) {
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                local.data() + dir.VirtualAddress + off);
            if (block->SizeOfBlock == 0) break;
            const size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            const auto* items = reinterpret_cast<const WORD*>(block + 1);
            for (size_t k = 0; k < count; ++k) {
                const WORD type = items[k] >> 12;
                const WORD pos = items[k] & 0xFFF;
                const size_t where = block->VirtualAddress + pos;
                if (where + 8 > image_size) continue;
                if (type == IMAGE_REL_BASED_DIR64) {
                    auto* p = reinterpret_cast<uint64_t*>(local.data() + where);
                    *p += delta;
                } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    auto* p = reinterpret_cast<uint32_t*>(local.data() + where);
                    *p += static_cast<uint32_t>(delta);
                }
            }
            off += block->SizeOfBlock;
        }
    }

    // 3) 导入表：只允许 KERNEL32.dll（系统 DLL 的基址全进程一致，所以本地取到的地址远程有效）
    const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.Size != 0) {
        size_t off = 0;
        while (true) {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                local.data() + imp_dir.VirtualAddress + off);
            if (desc->Name == 0) break;
            const char* dll_name = reinterpret_cast<const char*>(local.data() + desc->Name);
            std::wstring wide(dll_name, dll_name + std::strlen(dll_name));
            HMODULE mod = GetModuleHandleW(wide.c_str());
            if (!mod) {
                err = std::string("导入的 DLL 未加载：") + dll_name;
                return false;
            }
            if (_stricmp(dll_name, "KERNEL32.dll") != 0 && _stricmp(dll_name, "kernel32.dll") != 0) {
                err = std::string("暂不支持非 kernel32 的导入：") + dll_name;
                return false;
            }
            const size_t thunk_rva = desc->FirstThunk ? desc->FirstThunk : desc->OriginalFirstThunk;
            const size_t orig_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            size_t t = 0;
            while (true) {
                const auto* orig = reinterpret_cast<const uint64_t*>(local.data() + orig_rva + t);
                if (*orig == 0) break;
                FARPROC fn = nullptr;
                if ((*orig & IMAGE_ORDINAL_FLAG64) != 0) {
                    fn = GetProcAddress(mod, reinterpret_cast<LPCSTR>(*orig & 0xFFFF));
                } else {
                    const char* name = reinterpret_cast<const char*>(local.data() + (*orig) + 2);
                    fn = GetProcAddress(mod, name);
                }
                if (!fn) {
                    err = "解析导入函数失败";
                    return false;
                }
                auto* slot = reinterpret_cast<uint64_t*>(local.data() + thunk_rva + t);
                *slot = reinterpret_cast<uint64_t>(fn);
                t += sizeof(uint64_t);
            }
            off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
        }
    }

    // 4) 一次性写进目标进程
    SIZE_T written = 0;
    if (!WriteProcessMemory(hp, remote, local.data(), image_size, &written) ||
        written != image_size) {
        err = "写入镜像失败";
        return false;
    }
    *base_out = remote_base;
    return true;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) &&
                    got == out.size();
    CloseHandle(h);
    return ok;
}

bool inject_runtime(uint32_t pid, const std::vector<uint8_t>& bootstrap_dll,
                    const std::vector<uint8_t>& managed_dll, const std::wstring& mono_dll,
                    const std::string& cfg_json, const std::wstring& log_path,
                    uint32_t* exit_code, std::string& err) {
    HANDLE hp = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hp) {
        err = "OpenProcess 失败（权限不足？试试以管理员运行）";
        return false;
    }
    uint64_t base = 0;
    if (!manual_map(hp, bootstrap_dll, &base, err)) {
        CloseHandle(hp);
        return false;
    }

    uint64_t remote_managed = 0;
    if (!remote_write(hp, managed_dll.data(), managed_dll.size(), &remote_managed, err)) {
        CloseHandle(hp);
        return false;
    }

    const IMAGE_DOS_HEADER* dos = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    if (!parse(bootstrap_dll, &dos, &nt, err)) {
        CloseHandle(hp);
        return false;
    }

    Params params{};
    copy_wstr(params.mono_dll, 512, mono_dll);
    params.managed_bytes = remote_managed;
    params.managed_len = static_cast<int32_t>(managed_dll.size());
    copy_str(params.asm_name, sizeof(params.asm_name), "SFSHanRuntime.dll");
    copy_str(params.type_ns, sizeof(params.type_ns), "SFSHanRuntime");
    copy_str(params.type_name, sizeof(params.type_name), "Injector");
    copy_str(params.method_name, sizeof(params.method_name), "Start");
    std::vector<uint8_t> cfg(cfg_json.begin(), cfg_json.end());
    cfg.push_back(0);
    uint64_t remote_cfg = 0;
    if (!remote_write(hp, cfg.data(), cfg.size(), &remote_cfg, err)) {
        CloseHandle(hp);
        return false;
    }
    params.cfg_bytes = remote_cfg;
    params.cfg_len = static_cast<int32_t>(cfg.size());
    copy_wstr(params.log_path, 512, log_path);

    uint64_t remote_params = 0;
    if (!remote_write(hp, &params, sizeof(params), &remote_params, err)) {
        CloseHandle(hp);
        return false;
    }

    const uint64_t entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    HANDLE th = CreateRemoteThread(
        hp, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry),
        reinterpret_cast<LPVOID>(remote_params), 0, nullptr);
    if (!th) {
        err = "CreateRemoteThread 失败";
        CloseHandle(hp);
        return false;
    }
    WaitForSingleObject(th, 15000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    CloseHandle(hp);
    if (exit_code) *exit_code = code;
    return true;
}

}  // namespace inject
