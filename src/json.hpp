#pragma once
#include "dumper.hpp"
#include <charconv>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace ud::json {
using Value = rapidjson::Value;
inline const Value& get(const Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) throw std::runtime_error(std::string("JSON 缺少字段：") + key);
    return v[key];
}
inline std::string text(const Value& v, const char* key, const std::string& fallback = {}) {
    if (!v.IsObject() || !v.HasMember(key)) return fallback;
    if (!v[key].IsString()) throw std::runtime_error(std::string("JSON 字段应为文本：") + key);
    return {v[key].GetString(), v[key].GetStringLength()};
}
inline uint64_t number(const Value& v) {
    if (v.IsUint64()) return v.GetUint64();
    if (!v.IsString()) throw std::runtime_error("JSON 整数格式错误");
    std::string s(v.GetString(), v.GetStringLength());
    uint64_t n{};
    const bool hex = s.starts_with("0x") || s.starts_with("0X");
    const auto begin = s.data() + (hex ? 2 : 0);
    auto [end, error] = std::from_chars(begin, s.data() + s.size(), n, hex ? 16 : 10);
    if (begin == end || error != std::errc{} || end != s.data() + s.size()) throw std::runtime_error("JSON 整数超出范围或含有无效字符");
    return n;
}
inline uint64_t number(const Value& v, const char* key) { return number(get(v, key)); }
inline const Value& array(const Value& v, const char* key) {
    const auto& value = get(v, key);
    if (!value.IsArray()) throw std::runtime_error(std::string("JSON 字段应为数组：") + key);
    return value;
}
inline bool flag(const Value& v, const char* key, bool fallback = false) {
    if (!v.IsObject() || !v.HasMember(key)) return fallback;
    if (!v[key].IsBool()) throw std::runtime_error(std::string("JSON 字段应为布尔值：") + key);
    return v[key].GetBool();
}
inline rapidjson::Document load(const fs::path& path) {
    const auto bytes = read_file(path, 16 * 1024 * 1024);
    if (bytes.empty()) throw std::runtime_error("JSON 文件为空");
    rapidjson::Document d;
    d.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (d.HasParseError()) throw std::runtime_error(std::string("JSON 无法解析：") + rapidjson::GetParseError_En(d.GetParseError()));
    if (!d.IsObject()) throw std::runtime_error("JSON 根节点应为对象");
    return d;
}
}
