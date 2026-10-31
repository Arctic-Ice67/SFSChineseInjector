#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "text.h"

namespace json {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    void skip_ws() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
            } else {
                break;
            }
        }
    }

    bool fail(const std::string& msg) {
        if (err.empty()) err = msg + " (偏移 " + std::to_string(i) + ")";
        return false;
    }

    bool parse_value(Value& out) {
        skip_ws();
        if (i >= s.size()) return fail("意外的文本结尾");
        const char c = s[i];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            out.type = Type::String;
            return parse_string(out.str);
        }
        if (c == 't' || c == 'f') {
            if (s.compare(i, 4, "true") == 0) {
                i += 4;
                out.type = Type::Bool;
                out.boolean = true;
                return true;
            }
            if (s.compare(i, 5, "false") == 0) {
                i += 5;
                out.type = Type::Bool;
                out.boolean = false;
                return true;
            }
            return fail("无法识别的字面量");
        }
        if (c == 'n') {
            if (s.compare(i, 4, "null") == 0) {
                i += 4;
                out.type = Type::Null;
                return true;
            }
            return fail("无法识别的字面量");
        }
        return parse_number(out);
    }

    bool parse_number(Value& out) {
        const size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                                s[i] == 'E' || s[i] == '-' || s[i] == '+')) {
            ++i;
        }
        if (i == start) return fail("不是数字");
        out.type = Type::Number;
        out.number = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t& out) {
        if (i + 4 > s.size()) return fail("\\u 转义不完整");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("\\u 转义含非法字符");
        }
        i += 4;
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        out.clear();
        ++i;  // 跳过起始引号
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return fail("转义不完整");
                const char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' &&
                            s[i + 1] == 'u') {
                            i += 2;
                            uint32_t lo = 0;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return fail("未知转义");
                }
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return fail("字符串未闭合");
    }

    bool parse_array(Value& out) {
        out.type = Type::Array;
        ++i;  // [
        skip_ws();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.items.push_back(std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("数组未闭合");
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == ']') {
                ++i;
                return true;
            }
            return fail("数组里出现意外字符");
        }
    }

    bool parse_object(Value& out) {
        out.type = Type::Object;
        ++i;  // {
        skip_ws();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            skip_ws();
            if (i >= s.size() || s[i] != '"') return fail("对象的键必须是字符串");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (i >= s.size() || s[i] != ':') return fail("缺少冒号");
            ++i;
            Value v;
            if (!parse_value(v)) return false;
            out.fields.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("对象未闭合");
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == '}') {
                ++i;
                return true;
            }
            return fail("对象里出现意外字符");
        }
    }
};

void write_escaped(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void write_number(double v, std::string& out) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        out += std::to_string(static_cast<long long>(v));
    } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        out += buf;
    }
}

void write_value(const Value& v, int indent, int depth, std::string& out) {
    const std::string pad(static_cast<size_t>(indent) * (depth + 1), ' ');
    const std::string pad_close(static_cast<size_t>(indent) * depth, ' ');
    const bool pretty = indent > 0;
    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Type::Number: write_number(v.number, out); break;
        case Type::String: write_escaped(v.str, out); break;
        case Type::Array:
            if (v.items.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            for (size_t i = 0; i < v.items.size(); ++i) {
                if (i) out += ",";
                if (pretty) {
                    out += "\n";
                    out += pad;
                }
                write_value(v.items[i], indent, depth + 1, out);
            }
            if (pretty) {
                out += "\n";
                out += pad_close;
            }
            out += "]";
            break;
        case Type::Object:
            if (v.fields.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            for (size_t i = 0; i < v.fields.size(); ++i) {
                if (i) out += ",";
                if (pretty) {
                    out += "\n";
                    out += pad;
                }
                write_escaped(v.fields[i].first, out);
                out += pretty ? ": " : ":";
                write_value(v.fields[i].second, indent, depth + 1, out);
            }
            if (pretty) {
                out += "\n";
                out += pad_close;
            }
            out += "}";
            break;
    }
}

}  // namespace

Value make_null() { return Value(); }
Value make_bool(bool v) {
    Value r;
    r.type = Type::Bool;
    r.boolean = v;
    return r;
}
Value make_number(double v) {
    Value r;
    r.type = Type::Number;
    r.number = v;
    return r;
}
Value make_string(const std::string& v) {
    Value r;
    r.type = Type::String;
    r.str = v;
    return r;
}
Value make_array() {
    Value r;
    r.type = Type::Array;
    return r;
}
Value make_object() {
    Value r;
    r.type = Type::Object;
    return r;
}
void set(Value& obj, const std::string& key, Value v) {
    if (!obj.is_object()) obj.type = Type::Object;
    if (Value* existing = obj.find(key)) {
        *existing = std::move(v);
        return;
    }
    obj.fields.emplace_back(key, std::move(v));
}
void push(Value& arr, Value v) {
    if (!arr.is_array()) arr.type = Type::Array;
    arr.items.push_back(std::move(v));
}
void push(Value& arr, const std::string& s) { push(arr, make_string(s)); }

bool parse(const std::string& text, Value& out, std::string& err) {
    Parser p(text);
    // 跳过 UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        p.i = 3;
    }
    if (!p.parse_value(out)) {
        err = p.err;
        return false;
    }
    p.skip_ws();
    if (p.i != text.size()) {
        err = "JSON 末尾有多余内容 (偏移 " + std::to_string(p.i) + ")";
        return false;
    }
    return true;
}

std::string write(const Value& v, int indent) {
    std::string out;
    write_value(v, indent, 0, out);
    return out;
}

}  // namespace json
