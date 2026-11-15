#include "dumper.hpp"
#include "json.hpp"
#include <windows.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ud {
namespace {
bool identifier(const std::string& s) {
    return !s.empty() && (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
        std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}
bool available(Status s) { return s == Status::Located || s == Status::Candidate; }
bool partial(const Result& r) {
    return r.cancelled || r.code_valid != r.code_total || !r.notes.empty() ||
        std::any_of(r.fields.begin(), r.fields.end(), [](const Field& f) { return f.status != Status::Located; }) ||
        std::any_of(r.recipes.begin(), r.recipes.end(), [](const Recipe& v) { return v.status != Status::Located; });
}
std::string unresolved(const Result& r) {
    std::ostringstream out;
    std::set<std::string> printed;
    for (const auto& f : r.fields) if (!f.hidden && !available(f.status)) {
        auto text = f.name + " : " + status_key(f.status);
        if (printed.insert(text).second) out << "  " << text << '\n';
    }
    for (const auto& recipe : r.recipes) if (!available(recipe.status)) out << "  " << recipe.id << " : dependencies not recovered\n";
    for (const auto& note : r.notes) if (printed.insert(note).second) out << "  " << note << '\n';
    return out.str();
}
void write_new(const fs::path& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("无法新建导出文件：" + utf8(path.filename().wstring()));
    DWORD written{};
    auto okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    auto flushed = FlushFileBuffers(file);
    CloseHandle(file);
    if (!okay || !flushed || written != text.size()) throw std::runtime_error("导出文件未完整写入，请检查剩余空间");
}
}

std::string result_version(const Result& r) {
    auto version = r.archive_version.empty()
        ? (r.version_source == "PE FileVersion" ? std::string{} : r.version)
        : r.archive_version;
    std::erase_if(version, [](unsigned char c) { return c < 32 || c == 127; });
    const auto first = version.find_first_not_of(' '), last = version.find_last_not_of(' ');
    return first == std::string::npos ? "unknown" : version.substr(first, std::min<size_t>(last - first + 1, 96));
}

std::string forum_text(const Result& r, bool bbcode) {
    std::ostringstream out;
    if (bbcode) out << "[code]\n";
    const auto version = !r.archive_version.empty() ? result_version(r) : r.version_source == "PE FileVersion" ? "unknown" : result_version(r);
    out << "unleashed dumper v2\ngame overwatch 2\ngame version " << version << '\n';
    if (!r.version.empty() && (r.version_source == "PE FileVersion" || r.version != version))
        out << (r.version_source == "PE FileVersion" ? "file version " : "input version ") << r.version << '\n';
    out << "region " << r.region << "\ndumped " << r.created_utc << "\nstatus " << (r.cancelled ? "cancelled / partial" : partial(r) ? "partial" : "complete") << '\n';
    out << "coverage code " << r.code_valid << '/' << r.code_total << '\n';
    std::vector<std::string> groups;
    for (const auto& f : r.fields) if (!f.hidden && std::find(groups.begin(), groups.end(), f.group) == groups.end()) groups.push_back(f.group);
    size_t width = 0;
    for (const auto& f : r.fields) if (!f.hidden) width = std::max(width, f.name.size());
    for (Status state : {Status::Located, Status::Candidate}) {
        bool heading = false;
        for (const auto& group : groups) {
            std::vector<const Field*> rows;
            for (const auto& f : r.fields) if (!f.hidden && f.group == group && f.status == state && (f.value || !f.text.empty())) rows.push_back(&f);
            if (rows.empty()) continue;
            if (state == Status::Candidate && !heading) { out << "\ncandidates (global uniqueness not established)\n"; heading = true; }
            out << '\n' << group << '\n';
            for (const auto* f : rows) out << "  " << std::left << std::setw(static_cast<int>(width + 2)) << f->name << r.value_text(*f) << '\n';
        }
    }
    for (const auto& recipe : r.recipes) if (available(recipe.status) && !recipe.body.empty()) {
        out << '\n' << recipe.name;
        if (recipe.status == Status::Candidate) out << " (candidate; code coverage incomplete)";
        out << '\n';
        std::istringstream lines(recipe.body);
        std::string line;
        while (std::getline(lines, line)) out << "  " << line << '\n';
    }
    const auto missing = unresolved(r);
    if (!missing.empty()) out << "\nunresolved\n" << missing;
    out << "\nvalidation\n  code extraction only; gameplay behavior not tested\n";
    if (bbcode) out << "[/code]\n";
    return out.str();
}

std::string engineering_text(const Result& r, Language language) {
    const auto t=[&](const char* zh,const char* en){return local_text(language,zh,en);};
    std::ostringstream out;
    out << t("# UN 更新参考\n\n","# UN update reference\n\n") << r.region << " · " << result_version(r) << " · " << r.created_utc << "\n\n";
    if(!r.version.empty() && (r.version!=result_version(r) || r.version_source=="PE FileVersion"))
        out << t("输入中的版本：","Version from input: ") << r.version << "\n\n";
    out << t("方法：","Method: ") << r.profile << t("。代码覆盖 ",". Code coverage ") << r.code_valid << " / " << r.code_total << t(" 字节。\n\n"," bytes.\n\n");
    out << t("字段值来自本次输入。代码定位没有代替读取或游戏行为验证；下面的文件位置是修改参考。\n\n","Values come from this input. Code extraction does not replace live-read or gameplay validation. Source locations below are update references.\n\n");
    out << t("## 字段\n\n","## Fields\n\n");
    for(const auto& f:r.fields) if(!f.hidden || !f.consumer.empty()) {
        out << f.name << " = " << (available(f.status)?r.value_text(f):t("未知","unknown")) << '\n';
        if(!f.consumer.empty()) out << "  UN V2: " << f.consumer << '\n';
        if(f.status!=Status::Located) out << "  " << (language==Language::English?status_key(f.status):status_text(f.status)) << ": " << diagnostic_text(language,f.note) << '\n';
        if(f.previous && f.value && *f.previous!=*f.value) out << t("  变化: ","  Changed: ") << hex(*f.previous) << " -> " << hex(*f.value) << '\n';
        out << '\n';
    }
    out << t("## 解码方法\n\n","## Decode methods\n\n");
    for(const auto& recipe:r.recipes) {
        out << "### " << recipe.name << "\n\n" << diagnostic_text(language,recipe.note) << "\n\n";
        if(!recipe.body.empty()) out << "```text\n" << recipe.body << "\n```\n\n";
    }
    out << t("## 对应位置与使用\n\n","## Source locations and usage\n\n");
    if(r.region=="CN") out << t("- CN 常量及解码：","- CN constants and decoding: ") << "`include/Game/Cn155284.hpp`.\n"
        << t("- Profile 字段：","- Profile fields: ") << "`include/Game/Offsets.hpp` / `kCnRuntimeProfile`.\n";
    else if(r.region=="WORLD") out << t("- World 解码：","- World decoding: ") << "`include/Game/RegionalLayout.hpp` / `World155284`.\n"
        << t("- Profile 字段：","- Profile fields: ") << "`include/Game/Offsets.hpp` / `kWorld155284RuntimeProfile`.\n";
    else if(r.region=="NEXON") out << t("- Nexon 解码按本地区指令核对；共享实现位置：","- Verify Nexon decoding against its own instructions; shared implementation: ") << "`include/Game/Cn155284.hpp`.\n"
        << t("- Profile 字段：","- Profile fields: ") << "`include/Game/Offsets.hpp` / `kNexon155284RuntimeProfile`.\n";
    out << t("- 三服的字段覆盖分别以本报告为准；同形公式不代表全套结构和 RVA 相同。\n","- This report defines coverage for the selected region; matching formulas do not imply identical layouts or RVAs.\n");
    out << t("- `offsets.generated.hpp` 是本次已定位参数的参考，未写回 UN。\n","- `offsets.generated.hpp` contains located parameters; it does not modify UN.\n")
        << t("- 变更解码时同时核对类型、读取链和参数，不能只替换 RVA。\n\n","- Review types, read chains and decode parameters together when updating.\n\n");
    out << t("## 待处理\n\n","## Pending\n\n");
    for(const auto& f:r.fields) if(!f.hidden && !available(f.status)) out << "- " << f.name << ": " << diagnostic_text(language,f.note) << '\n';
    for(const auto& note:r.notes) out << "- " << diagnostic_text(language,note) << '\n';
    out << t("- 现场读取、相机运动，以及实体/Health/方向/英雄等所用业务的行为验证。\n","- Live reads, camera motion and entity / Health / rotation / hero behavior validation.\n");
    return out.str();
}

std::string reference_header(const Result& r) {
    std::ostringstream out;
    out << "#pragma once\n#include <array>\n#include <cstdint>\n\n"
        << "// Generated from code extraction; no gameplay validation or automatic UN update.\n"
        << "// " << r.region << " / " << result_version(r) << " / " << r.created_utc << "\n"
        << "namespace unleashed_dump {\n";
    std::set<std::string> names;
    for (const auto& f : r.fields) if (f.status == Status::Located && f.value) {
        if (!identifier(f.name) || !names.insert(f.name).second) throw std::runtime_error("导出常量名称无效或重复");
        out << "inline constexpr std::uint64_t " << f.name << " = " << hex(*f.value) << "ULL;";
        if (!f.consumer.empty()) out << " // " << f.consumer;
        out << '\n';
    }
    out << "}\n";
    return out.str();
}

std::string result_json(const Result& r) {
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(buffer);
    auto string = [&](const char* key, const std::string& value) { w.Key(key); w.String(value.data(), static_cast<rapidjson::SizeType>(value.size())); };
    w.StartObject(); string("schema", "unleashed-dumper-result/2");
    string("region", r.region); string("profile", r.profile); string("version", r.version); string("version_source", r.version_source);
    string("archive_version", r.archive_version);
    string("created_utc", r.created_utc); string("source_kind", r.source_kind); string("source_name", r.source_name);
    w.Key("timestamp"); w.Uint(r.timestamp); w.Key("image_size"); w.Uint(r.image_size);
    w.Key("code_valid"); w.Uint64(r.code_valid); w.Key("code_total"); w.Uint64(r.code_total); w.Key("cancelled"); w.Bool(r.cancelled);
    w.Key("fields"); w.StartArray();
    for (const auto& f : r.fields) {
        w.StartObject(); string("id", f.id); string("name", f.name); string("group", f.group); string("kind", f.kind); string("consumer", f.consumer);
        string("note", f.note); string("text", f.text); string("status", status_key(f.status));
        w.Key("hidden"); w.Bool(f.hidden); w.Key("value"); if (f.value) w.Uint64(*f.value); else w.Null();
        w.Key("previous"); if (f.previous) w.Uint64(*f.previous); else w.Null();
        w.Key("evidence"); w.StartArray();
        for (const auto& e : f.evidence) { w.StartObject(); w.Key("rva"); w.Uint64(e.rva); string("instruction", e.instruction); string("bytes", e.bytes); string("rule", e.rule); w.EndObject(); }
        w.EndArray(); w.EndObject();
    }
    w.EndArray(); w.Key("recipes"); w.StartArray();
    for (const auto& recipe : r.recipes) {
        w.StartObject(); string("id", recipe.id); string("name", recipe.name); string("body", recipe.body); string("note", recipe.note); string("status", status_key(recipe.status));
        w.Key("dependencies"); w.StartArray(); for (const auto& d : recipe.dependencies) w.String(d.c_str()); w.EndArray(); w.EndObject();
    }
    w.EndArray(); w.Key("notes"); w.StartArray(); for (const auto& note : r.notes) w.String(note.c_str()); w.EndArray(); w.EndObject();
    return {buffer.GetString(), buffer.GetSize()};
}

Result load_result(const fs::path& path) {
    auto d = json::load(path);
    if (json::text(d, "schema") != "unleashed-dumper-result/2") throw std::runtime_error("这不是 V2 扫描结果；采集报告请作为扫描输入打开");
    Result r;
    r.region = json::text(d, "region"); r.profile = json::text(d, "profile"); r.version = json::text(d, "version"); r.version_source = json::text(d, "version_source");
    r.archive_version = json::text(d, "archive_version");
    if (r.region != "CN" && r.region != "WORLD" && r.region != "NEXON") throw std::runtime_error("结果的地区名称无效");
    r.created_utc = json::text(d, "created_utc"); r.source_kind = json::text(d, "source_kind"); r.source_name = json::text(d, "source_name");
    auto timestamp = json::number(d, "timestamp"), size = json::number(d, "image_size");
    if (timestamp > UINT32_MAX || size > 512ull * 1024 * 1024) throw std::runtime_error("结果的 PE 身份无效");
    r.timestamp = static_cast<uint32_t>(timestamp); r.image_size = static_cast<uint32_t>(size);
    r.code_valid = json::number(d, "code_valid"); r.code_total = json::number(d, "code_total"); r.cancelled = json::flag(d, "cancelled");
    if (r.code_valid > r.code_total || r.code_total > r.image_size) throw std::runtime_error("结果的代码覆盖范围无效");
    const auto& fields = json::get(d, "fields"), &recipes = json::get(d, "recipes"), &notes = json::get(d, "notes");
    if (!fields.IsArray() || fields.Size() > 512 || !recipes.IsArray() || recipes.Size() > 64 || !notes.IsArray()) throw std::runtime_error("结果列表格式无效");
    std::set<std::string> identities;
    for (const auto& v : fields.GetArray()) {
        Field f;
        f.id = json::text(v, "id"); f.name = json::text(v, "name"); f.group = json::text(v, "group"); f.kind = json::text(v, "kind"); f.consumer = json::text(v, "consumer");
        if (!identifier(f.id) || !identifier(f.name) || !identities.insert(f.id).second) throw std::runtime_error("结果字段名称无效或重复");
        f.note = json::text(v, "note"); f.text = json::text(v, "text"); f.hidden = json::flag(v, "hidden"); f.status = parse_status(json::text(v, "status"));
        const auto& value = json::get(v, "value"), &previous = json::get(v, "previous");
        if (!value.IsNull()) f.value = json::number(value);
        if (!previous.IsNull()) f.previous = json::number(previous);
        if (available(f.status) && !f.value && f.text.empty()) throw std::runtime_error("已定位字段缺少值");
        const auto& evidence = json::get(v, "evidence");
        if (!evidence.IsArray() || evidence.Size() > 256) throw std::runtime_error("结果证据列表无效");
        for (const auto& e : evidence.GetArray()) f.evidence.push_back({json::number(e, "rva"), json::text(e, "instruction"), json::text(e, "bytes"), json::text(e, "rule")});
        r.fields.push_back(std::move(f));
    }
    for (const auto& v : recipes.GetArray()) {
        Recipe recipe;
        recipe.id = json::text(v, "id"); recipe.name = json::text(v, "name"); recipe.body = json::text(v, "body"); recipe.note = json::text(v, "note"); recipe.status = parse_status(json::text(v, "status"));
        if (!identifier(recipe.id)) throw std::runtime_error("结果解码名称无效");
        const auto& deps = json::get(v, "dependencies");
        if (!deps.IsArray() || deps.Size() > 256) throw std::runtime_error("结果解码依赖列表无效");
        for (const auto& x : deps.GetArray()) { if (!x.IsString()) throw std::runtime_error("结果依赖名称无效"); recipe.dependencies.push_back(x.GetString()); }
        r.recipes.push_back(std::move(recipe));
    }
    for (const auto& v : notes.GetArray()) { if (!v.IsString()) throw std::runtime_error("结果说明应为文本"); r.notes.emplace_back(v.GetString(), v.GetStringLength()); }
    return r;
}

fs::path save_bundle(const Result& r, const fs::path& parent, Language language) {
    const auto text = forum_text(r), engineering = engineering_text(r, language), header = reference_header(r), saved = result_json(r);
    auto version = result_version(r);
    for (auto& c : version) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
    while (!version.empty() && version.back() == '.') version.pop_back();
    const auto collection = parent / wide(r.region + "-" + version);
    fs::create_directories(collection);
    std::string name = r.created_utc;
    for (auto& c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '-';
    for (unsigned index = 0; index < 1000; ++index) {
        const auto path = collection / wide(name + (index ? "-" + std::to_string(index) : ""));
        if (!fs::create_directory(path)) continue;
        write_new(path / "forum.txt", text); write_new(path / "UN_UPDATE.md", engineering);
        write_new(path / "offsets.generated.hpp", header); write_new(path / "result.json", saved);
        return path;
    }
    throw std::runtime_error("无法建立新的导出目录");
}
}
