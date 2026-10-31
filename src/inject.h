// 手动映射（manual map）：把 DLL 字节直接映射进目标进程，全程不落盘。
// 只支持 x64、导入只允许 KERNEL32.dll（我们的引导 DLL 就是这么编的）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inject {

// 找到游戏进程 pid（按进程名匹配，忽略大小写）。0 = 没找到
uint32_t find_process(const std::wstring& exe_name);

// 等目标进程里出现某个模块（比如 mono-2.0-bdwgc.dll），返回是否等到
bool wait_for_module(uint32_t pid, const wchar_t* module_name, uint32_t timeout_ms);

// 把 image 手动映射进 hproc，返回远程基址
bool manual_map(void* hproc, const std::vector<uint8_t>& image, uint64_t* base_out, std::string& err);

// 在目标进程分配内存并写入
bool remote_write(void* hproc, const void* data, size_t len, uint64_t* out_addr, std::string& err);

// 完整流程：映射引导 DLL → 写入托管 DLL 字节和参数 → 起远程线程调用 HanBootstrap
// mono_dll 是 mono-2.0-bdwgc.dll 的绝对路径；mod_folder 是 UTF-8 的运行时 mod 目录
bool inject_runtime(uint32_t pid, const std::vector<uint8_t>& bootstrap_dll,
                    const std::vector<uint8_t>& managed_dll, const std::wstring& mono_dll,
                    const std::string& cfg_json, const std::wstring& log_path,
                    uint32_t* exit_code, std::string& err);

// 读文件到字节
bool read_file(const std::wstring& path, std::vector<uint8_t>& out);

}  // namespace inject
