// 极简 JSON：够用即可（词表 / 清单 / 配置都是我们自己生成和读取的）。
// 支持 \uXXXX 转义（会转成 UTF-8），输出时中文原样写、控制字符转义。
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;                          // Array
    std::vector<std::pair<std::string, Value>> fields;  // Object（保持插入顺序）

    bool is_null() const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }

    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : fields) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    Value* find(const std::string& key) {
        if (type != Type::Object) return nullptr;
        for (auto& kv : fields) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    std::string as_string(const std::string& def = std::string()) const {
        return type == Type::String ? str : def;
    }
    double as_number(double def = 0.0) const { return type == Type::Number ? number : def; }
    bool as_bool(bool def = false) const { return type == Type::Bool ? boolean : def; }
    int as_int(int def = 0) const { return type == Type::Number ? static_cast<int>(number) : def; }

    // 便捷取值（找不到返回默认）
    std::string get_string(const std::string& key, const std::string& def = std::string()) const {
        const Value* v = find(key);
        return v ? v->as_string(def) : def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        const Value* v = find(key);
        return v ? v->as_bool(def) : def;
    }
    int get_int(const std::string& key, int def = 0) const {
        const Value* v = find(key);
        return v ? v->as_int(def) : def;
    }
};

Value make_null();
Value make_bool(bool v);
Value make_number(double v);
Value make_string(const std::string& v);
Value make_array();
Value make_object();
void set(Value& obj, const std::string& key, Value v);
void push(Value& arr, Value v);
void push(Value& arr, const std::string& s);

bool parse(const std::string& text, Value& out, std::string& err);
std::string write(const Value& v, int indent = 2);

}  // namespace json
