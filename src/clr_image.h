// .NET 程序集（DLL）只读解析 + #US 字面量原地改写。
//
// 设计要点（都是踩过的坑）：
//   * #US 条目布局：压缩长度前缀(含末尾 flag 字节) + UTF-16LE 内容 + 1 字节 flag。
//     条目之间没有对齐填充 —— 少加/多加 1 字节会让后面全部错位（解码出乱码）。
//   * ldstr 的 IL 操作码是 0x72（不是 0x70），操作数是 4 字节 token = 0x70000000 | #US偏移。
//   * 改字符串必须保持 nbytes 不变：中文字节数不足时用 U+200B 补满，
//     这样长度前缀、后续条目偏移、metadata 布局全都不用动，结构风险为零。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clr {

struct Section {
    std::string name;
    uint32_t va = 0;
    uint32_t vsize = 0;
    uint32_t raw = 0;
    uint32_t rsize = 0;
};

struct UserString {
    uint32_t heap_off = 0;    // #US 堆内偏移（= ldstr token 低 24 位）
    uint32_t content_at = 0;  // 文件偏移：UTF-16 内容起点
    uint32_t nbytes = 0;      // 内容字节数（不含末尾 flag 字节）
    uint32_t flag_at = 0;     // 文件偏移：末尾 flag 字节
    std::u16string text;
    std::vector<uint32_t> sites;     // ldstr 操作数在文件中的偏移
    std::vector<std::string> owners; // 引用它的方法（Namespace.Type::Method），最多 8 条
    bool in_strings_heap = false;    // 同样文本也作为标识符出现在 #Strings ⇒ 高风险
};

class Image {
public:
    bool load(const std::string& path, std::string& err);
    bool save_as(const std::string& path, std::string& err) const;

    const std::vector<uint8_t>& bytes() const { return data_; }
    const std::vector<UserString>& strings() const { return strings_; }
    size_t method_count() const { return method_rvas_.size(); }
    const std::string& path() const { return path_; }
    const std::string& assembly_name() const { return assembly_name_; }
    size_t ldstr_count() const { return ldstr_count_; }

    // 用 UTF-16 文本原地覆写第 index 条字面量。
    // 新文本字节数必须 <= 原字节数（不足用 U+200B 补满）。
    bool replace(size_t index, const std::u16string& text, std::string& err);

    // 结构自检：重新解析当前字节、逐条比对偏移/长度/引用点，并确认只改了预期区间。
    bool self_check(std::string& err) const;

    // 判断某个文件区间是否被改过（用于自检）
    bool diff_ranges(std::vector<std::pair<uint32_t, uint32_t>>& ranges) const;

private:
    bool parse(std::string& err);
    uint32_t rva_to_off(uint32_t rva, bool* ok = nullptr) const;
    bool read_tables(std::string& err);
    bool read_user_strings(std::string& err);
    bool read_strings_heap();
    bool scan_ldstr();
    bool read_assembly_name();

    std::string path_;
    std::string assembly_name_;
    std::vector<uint8_t> data_;
    std::vector<uint8_t> original_;
    std::vector<Section> sections_;

    uint32_t md_off_ = 0;       // metadata root 文件偏移
    uint32_t us_off_ = 0;       // #US 文件偏移
    uint32_t us_size_ = 0;
    uint32_t strings_off_ = 0;  // #Strings 文件偏移
    uint32_t strings_size_ = 0;
    uint32_t tables_off_ = 0;   // #~ 文件偏移
    uint32_t tables_size_ = 0;

    std::vector<uint32_t> method_rvas_;
    std::vector<std::string> method_names_;  // Namespace.Type::Method
    std::vector<UserString> strings_;
    std::vector<std::string> identifiers_;   // #Strings 里的全部名字
    size_t ldstr_count_ = 0;

    // 原始快照，用于 self_check / diff_ranges
    struct Snapshot {
        uint32_t heap_off = 0;
        uint32_t nbytes = 0;
        uint32_t content_at = 0;
        size_t site_count = 0;
    };
    std::vector<Snapshot> snapshot_;
};

}  // namespace clr
