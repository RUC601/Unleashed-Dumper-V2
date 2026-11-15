#include "dumper.hpp"
#include "json.hpp"
#include <windows.h>
#include <capstone/capstone.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ud {
namespace {
std::string canonical_region(std::string region) {
    std::transform(region.begin(), region.end(), region.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (region == "NE" || region == "NETEASE") return "CN";
    if (region == "BZ" || region == "US" || region == "EU") return "WORLD";
    if (region == "KR") return "NEXON"; // Historical product profile name, not a version-service row.
    return region;
}
struct Pattern {
    std::vector<int> bytes;
    Bytes anchor;
    size_t anchor_offset{};
    explicit Pattern(const std::string& text) {
        std::istringstream in(text);
        std::string token;
        size_t start{};
        Bytes run;
        auto finish = [&] {
            if (run.size() > anchor.size()) { anchor = run; anchor_offset = start; }
            run.clear();
        };
        while (in >> token) {
            if (token == "?" || token == "??") { bytes.push_back(-1); finish(); continue; }
            unsigned byte{};
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), byte, 16);
            if (token.size() != 2 || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || byte > 255)
                throw std::runtime_error("扫描规则含无效特征字节");
            if (run.empty()) start = bytes.size();
            run.push_back(static_cast<uint8_t>(byte)); bytes.push_back(static_cast<int>(byte));
        }
        finish();
        if (bytes.size() < 4 || bytes.size() > 16384 || anchor.size() < 2 || std::count_if(bytes.begin(), bytes.end(), [](int b) { return b >= 0; }) < 4)
            throw std::runtime_error("扫描规则缺少足够的固定字节");
    }
};
struct Hits {
    std::vector<uint64_t> rvas;
    size_t length{};
    bool capped{};
};
Hits find(const Image& image, const Pattern& pattern, std::stop_token stop) {
    Hits hits;
    hits.length = pattern.bytes.size();
    for (const auto& section : image.sections) if (section.flags & IMAGE_SCN_MEM_EXECUTE) for (auto span : section.valid) {
        const size_t end = static_cast<size_t>(span.offset) + span.size;
        for (size_t chunk = span.offset; chunk < end; chunk += 1024 * 1024) {
            if (stop.stop_requested()) return hits;
            const auto chunk_end = std::min(end, chunk + 1024 * 1024);
            const auto search_end = std::min(end, chunk_end + pattern.anchor.size() - 1);
            auto cursor = section.bytes.begin() + chunk;
            const auto limit = section.bytes.begin() + search_end;
            while (cursor != limit) {
                auto found = std::search(cursor, limit, pattern.anchor.begin(), pattern.anchor.end());
                if (found == limit) break;
                const size_t anchor_at = static_cast<size_t>(found - section.bytes.begin());
                if (anchor_at >= chunk_end) break;
                cursor = found + 1;
                if (anchor_at < pattern.anchor_offset) continue;
                const size_t candidate = anchor_at - pattern.anchor_offset;
                if (candidate < span.offset || candidate > end || pattern.bytes.size() > end - candidate) continue;
                bool matches = true;
                for (size_t i = 0; i < pattern.bytes.size(); ++i) if (pattern.bytes[i] >= 0 && section.bytes[candidate + i] != pattern.bytes[i]) { matches = false; break; }
                if (!matches) continue;
                if (hits.rvas.size() == 16) { hits.capped = true; return hits; }
                hits.rvas.push_back(uint64_t(section.rva) + candidate);
            }
        }
    }
    return hits;
}

class Decoder {
    HMODULE dll_{};
    csh handle_{};
    decltype(&cs_open) open_{};
    decltype(&cs_option) option_{};
    decltype(&cs_disasm) disasm_{};
    decltype(&cs_free) free_{};
    decltype(&cs_close) close_{};
    template<class T> T symbol(const char* name) {
        auto pointer = GetProcAddress(dll_, name);
        if (!pointer) throw std::runtime_error(std::string("Capstone 缺少函数：") + name);
        return reinterpret_cast<T>(pointer);
    }
public:
    explicit Decoder(const fs::path& runtime) {
        dll_ = LoadLibraryExW((runtime / "capstone.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll_) throw std::runtime_error("无法加载程序目录中的 Capstone 原生库");
        try {
            open_ = symbol<decltype(open_)>("cs_open"); option_ = symbol<decltype(option_)>("cs_option");
            disasm_ = symbol<decltype(disasm_)>("cs_disasm"); free_ = symbol<decltype(free_)>("cs_free"); close_ = symbol<decltype(close_)>("cs_close");
            if (open_(CS_ARCH_X86, CS_MODE_64, &handle_) != CS_ERR_OK || option_(handle_, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
                throw std::runtime_error("Capstone 无法建立 x64 解码器");
        } catch (...) { if (handle_ && close_) close_(&handle_); FreeLibrary(dll_); dll_ = nullptr; throw; }
    }
    ~Decoder() { if (handle_) close_(&handle_); if (dll_) FreeLibrary(dll_); }
    cs_insn one(const Image& image, uint64_t rva, size_t remaining, cs_detail& detail) {
        auto bytes = image.at(rva, std::min<size_t>(15, remaining));
        if (bytes.empty()) throw std::runtime_error("指令窗口未完整读取");
        cs_insn* decoded{};
        const auto count = disasm_(handle_, bytes.data(), bytes.size(), rva, 1, &decoded);
        if (!count || !decoded->detail) throw std::runtime_error("指令无法解码");
        auto instruction = *decoded;
        detail = *decoded->detail;
        free_(decoded, count);
        instruction.detail = &detail;
        return instruction;
    }
};

Evidence evidence(const cs_insn& instruction, const std::string& rule) {
    Evidence result;
    result.rva = instruction.address; result.rule = rule;
    result.instruction = std::string(instruction.mnemonic) + " " + instruction.op_str;
    std::ostringstream bytes;
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < instruction.size; ++i) { if (i) bytes << ' '; bytes << digits[instruction.bytes[i] >> 4] << digits[instruction.bytes[i] & 15]; }
    result.bytes = bytes.str();
    return result;
}

void extract(Field& field, const json::Value& spec, const Hits& hits, const Image& image, Decoder& decoder) {
    const auto mode = json::text(spec, "extract") == "referenced" ? json::text(spec, "operand") : json::text(spec, "extract");
    const auto rule = json::text(spec, "rule", "referenced:" + json::text(spec, "reference"));
    if (mode == "match") { field.value = hits.rvas[0]; return; }
    if (mode == "lut") {
        std::vector<uint8_t> table;
        const auto& offsets = json::get(spec, "instructions");
        if (!offsets.IsArray() || offsets.Size() % 4 || offsets.Size() > 256) throw std::runtime_error("替换表指令列表无效");
        for (const auto& offset : offsets.GetArray()) {
            const auto off = json::number(offset);
            if (off >= hits.length) throw std::runtime_error("替换表指令越界");
            cs_detail detail{};
            auto i = decoder.one(image, hits.rvas[0] + off, hits.length - static_cast<size_t>(off), detail);
            const auto& x = detail.x86;
            if (i.id != X86_INS_MOV || x.op_count != 2 || x.operands[0].type != X86_OP_MEM || x.operands[1].type != X86_OP_IMM || x.encoding.imm_size != 4)
                throw std::runtime_error("替换表初始化指令改变");
            const auto value = static_cast<uint32_t>(x.operands[1].imm);
            for (unsigned b = 0; b < 4; ++b) table.push_back(static_cast<uint8_t>(value >> (b * 8)));
            if (field.evidence.size() < 4) field.evidence.push_back(evidence(i, rule));
        }
        if (table.size() < 16 || table.size() % 16) throw std::runtime_error("替换表不完整");
        std::vector<uint8_t> sorted(table.begin(), table.begin() + 16);
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) if (sorted[i] != i) throw std::runtime_error("替换表不是 4 位排列");
        for (size_t i = 16; i < table.size(); ++i) if (table[i] != table[i % 16]) throw std::runtime_error("各替换表初始化值不一致");
        field.text = "{";
        for (size_t i = 0; i < 16; ++i) { if (i) field.text += ", "; field.text += std::to_string(table[i]); }
        field.text += "}";
        return;
    }
    const auto offset = json::number(spec, "offset");
    if (offset >= hits.length) throw std::runtime_error("规则的提取指令越界");
    cs_detail detail{};
    auto i = decoder.one(image, hits.rvas[0] + offset, hits.length - static_cast<size_t>(offset), detail);
    const auto expected = json::text(spec, "mnemonic");
    if (!expected.empty() && expected != i.mnemonic) throw std::runtime_error("提取指令种类改变");
    const auto& x = detail.x86;
    std::vector<uint64_t> values;
    for (size_t o = 0; o < x.op_count; ++o) {
        const auto& op = x.operands[o];
        if (mode == "rip" && op.type == X86_OP_MEM && op.mem.base == X86_REG_RIP) {
            const auto target = static_cast<int64_t>(i.address + i.size) + op.mem.disp;
            if (target < 0 || static_cast<uint64_t>(target) >= image.image_size) throw std::runtime_error("相对引用超出当前模块");
            values.push_back(static_cast<uint64_t>(target));
        } else if (mode == "imm" && op.type == X86_OP_IMM) values.push_back(static_cast<uint64_t>(op.imm));
        else if (mode == "mem" && op.type == X86_OP_MEM && op.mem.base != X86_REG_RIP) {
            if (op.mem.disp < 0) throw std::runtime_error("结构成员偏移为负");
            values.push_back(static_cast<uint64_t>(op.mem.disp));
        }
    }
    if (values.size() != 1) throw std::runtime_error("提取操作数缺失或有歧义");
    field.value = values[0]; field.evidence.push_back(evidence(i, rule));
}
std::string substitute(const std::string& body, const Result& result, std::vector<std::string>& dependencies, Status& status) {
    std::string out;
    size_t at{};
    while (at < body.size()) {
        auto start = body.find("${", at);
        if (start == std::string::npos) { out += body.substr(at); break; }
        out += body.substr(at, start - at);
        const auto end = body.find('}', start + 2);
        if (end == std::string::npos) throw std::runtime_error("解码模板格式错误");
        const auto id = body.substr(start + 2, end - start - 2);
        const auto* field = result.field(id);
        dependencies.push_back(id);
        if (!field || (field->status != Status::Located && field->status != Status::Candidate) || (!field->value && field->text.empty())) {
            status = Status::Unsupported;
            return {};
        }
        if (field->status == Status::Candidate) status = Status::Candidate;
        out += result.value_text(*field);
        at = end + 1;
    }
    return out;
}
}

std::string status_text(Status s) {
    switch (s) {
    case Status::Located: return "已定位";
    case Status::Candidate: return "局部候选";
    case Status::Ambiguous: return "多个候选";
    case Status::Missing: return "未找到";
    default: return "未支持";
    }
}
std::string status_key(Status s) {
    switch (s) {
    case Status::Located: return "located";
    case Status::Candidate: return "candidate";
    case Status::Ambiguous: return "ambiguous";
    case Status::Missing: return "missing";
    default: return "unsupported";
    }
}
Status parse_status(const std::string& s) {
    if (s == "located") return Status::Located;
    if (s == "candidate") return Status::Candidate;
    if (s == "ambiguous") return Status::Ambiguous;
    if (s == "missing") return Status::Missing;
    if (s == "unsupported") return Status::Unsupported;
    throw std::runtime_error("保存结果中的字段状态无效");
}
const Field* Result::field(const std::string& id) const {
    const auto it = std::find_if(fields.begin(), fields.end(), [&](const Field& f) { return f.id == id; });
    return it == fields.end() ? nullptr : &*it;
}
std::string Result::value_text(const Field& field) const { return field.value ? hex(*field.value) : field.text; }
size_t Result::located_count() const {
    return std::count_if(fields.begin(), fields.end(), [](const Field& f) { return !f.hidden && f.status == Status::Located; });
}

Result scan(const Image& image, const fs::path& rules, const fs::path& runtime, const std::string& region, std::stop_token stop, const Progress& progress) {
    if (region != "CN" && region != "WORLD" && region != "NEXON") throw std::runtime_error("请选择 CN、World 或 Nexon");
    const auto hint = canonical_region(image.region_hint);
    if (!hint.empty() && hint != region)
        throw std::runtime_error("Capture region " + hint + " does not match selected region " + region);
    Result result;
    result.source_name = utf8(image.source.filename().wstring()); result.source_kind = image.source_kind;
    result.created_utc = utc_now(); result.version = image.version; result.version_source = image.version_source;
    result.timestamp = image.timestamp; result.image_size = image.image_size; result.notes = image.notes;
    for (const auto& section : image.sections) if (section.flags & IMAGE_SCN_MEM_EXECUTE) { result.code_total += section.size; result.code_valid += section.valid_bytes(); }
    auto catalog = json::load(rules);
    if (json::text(catalog, "schema") != "unleashed-dumper-rules/2") throw std::runtime_error("规则文件版本不匹配");
    // Recognized evidence can reject a known wrong line. Unknown/new builds
    // still reach extraction; this is not a timestamp-based version lock.
    if (catalog.HasMember("known_images")) {
        const auto& identities = json::array(catalog, "known_images");
        if (identities.Size() > 128) throw std::runtime_error("Too many known image identities");
        std::set<std::string> regions;
        for (const auto& identity : identities.GetArray())
            if (json::number(identity, "timestamp") == image.timestamp && json::number(identity, "image_size") == image.image_size) {
                const auto known = canonical_region(json::text(identity, "region"));
                if (known != "CN" && known != "WORLD" && known != "NEXON") throw std::runtime_error("Invalid known image region");
                regions.insert(known);
            }
        if (regions.size() == 1 && *regions.begin() != region)
            throw std::runtime_error("Known input region " + *regions.begin() + " does not match selected region " + region);
    }
    const auto& profiles = json::get(catalog, "profiles");
    if (!profiles.IsArray() || profiles.Empty() || profiles.Size() > 8) throw std::runtime_error("规则配置无效");
    const json::Value* selected = nullptr;
    for (const auto& candidate : profiles.GetArray()) if (json::text(candidate, "region") == region) {
        if (selected) throw std::runtime_error("同一地区存在重复规则配置");
        selected = &candidate;
    }
    if (!selected) {
        result.region = region; result.profile = "not connected";
        result.cancelled = stop.stop_requested() || image.cancelled;
        result.notes.push_back(region + " extraction methods not yet connected; no other region's rules were used");
        if (progress) progress(1.f, "当前线路的提取方法尚未接入");
        return result;
    }
    const auto& profile = *selected;
    result.region = json::text(profile, "region"); result.profile = json::text(profile, "name");
    Decoder decoder(runtime);
    std::map<std::string, Hits> matches;
    std::map<std::string, Pattern> compiled;
    const auto& pattern_rows = json::get(profile, "patterns");
    if (!pattern_rows.IsArray() || pattern_rows.Size() > 128) throw std::runtime_error("规则特征列表无效");
    size_t index{};
    for (const auto& row : pattern_rows.GetArray()) {
        if (stop.stop_requested() || image.cancelled) break;
        const auto id = json::text(row, "id");
        if (progress) progress(.2f + .5f * static_cast<float>(index) / std::max(1u, pattern_rows.Size()), "定位 " + json::text(row, "label", id));
        auto [pattern, inserted] = compiled.emplace(id, Pattern(json::text(row, "pattern")));
        if (!inserted) throw std::runtime_error("扫描规则名称重复");
        matches.emplace(id, find(image, pattern->second, stop));
        ++index;
    }
    // Associate repeated helper blocks with a unique method owner; never pick the first global hit.
    if (profile.HasMember("constraints") && !stop.stop_requested()) {
        const auto& constraints = json::array(profile, "constraints");
        const auto verifies = [&](const std::string& id, uint64_t rva) {
            const auto pattern = compiled.find(id);
            if (pattern == compiled.end()) return false;
            auto code = image.at(rva, pattern->second.bytes.size());
            if (code.empty()) return false;
            for (size_t i = 0; i < code.size(); ++i) if (pattern->second.bytes[i] >= 0 && code[i] != pattern->second.bytes[i]) return false;
            return true;
        };
        for (size_t pass = 0; pass <= constraints.Size(); ++pass) for (const auto& c : constraints.GetArray()) {
            auto a = matches.find(json::text(c, "left_rule")), b = matches.find(json::text(c, "right_rule"));
            if (a == matches.end() || b == matches.end()) continue;
            const auto distance = json::number(c, "distance");
            if (b->second.rvas.size() == 1 && !b->second.capped && b->second.rvas[0] >= distance) {
                const auto rva = b->second.rvas[0] - distance;
                if (verifies(a->first, rva)) { a->second.rvas = {rva}; a->second.capped = false; }
            }
            if (a->second.rvas.size() == 1 && !a->second.capped && distance < image.image_size && a->second.rvas[0] <= image.image_size - distance) {
                const auto rva = a->second.rvas[0] + distance;
                if (verifies(b->first, rva)) { b->second.rvas = {rva}; b->second.capped = false; }
            }
        }
    }
    const bool complete = image.code_complete() && !stop.stop_requested() && !image.cancelled;
    const auto& fields = json::get(profile, "fields");
    if (!fields.IsArray() || fields.Size() > 512) throw std::runtime_error("规则字段列表无效");
    for (const auto& spec : fields.GetArray()) {
        Field field;
        field.id = json::text(spec, "id"); field.name = json::text(spec, "name", field.id); field.group = json::text(spec, "group");
        field.kind = json::text(spec, "kind"); field.consumer = json::text(spec, "consumer"); field.hidden = json::flag(spec, "hidden");
        if (spec.HasMember("baseline")) field.previous = json::number(spec, "baseline");
        const auto mode = json::text(spec, "extract");
        if (mode == "difference") {
            const auto* a = result.field(json::text(spec, "left")), *b = result.field(json::text(spec, "right"));
            if (a && b && a->value && b->value && *a->value >= *b->value &&
                (a->status == Status::Located || a->status == Status::Candidate) && (b->status == Status::Located || b->status == Status::Candidate)) {
                field.value = *a->value - *b->value;
                field.status = a->status == Status::Located && b->status == Status::Located ? Status::Located : Status::Candidate;
                field.note = "由本次两个字段的相对位置计算";
                field.evidence = a->evidence;
            } else field.note = "所依赖的字段尚未定位";
        } else if (mode == "referenced") {
            const auto* reference = result.field(json::text(spec, "reference"));
            if (!reference || !reference->value || (reference->status != Status::Located && reference->status != Status::Candidate)) {
                field.status = Status::Unsupported;
                field.note = "Referenced helper target was not recovered";
            } else try {
                const auto operand = json::text(spec, "operand");
                if (operand != "imm" && operand != "rip" && operand != "mem") throw std::runtime_error("Unsupported referenced extraction operand");
                const Pattern pattern(json::text(spec, "pattern"));
                const auto target = *reference->value;
                if (target >= image.image_size || pattern.bytes.size() > image.image_size - target)
                    throw std::runtime_error("Referenced helper is outside the module");
                if (std::none_of(image.sections.begin(), image.sections.end(), [&](const Section& section) {
                    return (section.flags & IMAGE_SCN_MEM_EXECUTE) && section.contains(target, pattern.bytes.size());
                })) throw std::runtime_error("Referenced helper is not fully stored executable code");
                const auto bytes = image.at(target, pattern.bytes.size());
                if (bytes.empty()) throw std::runtime_error("Referenced helper window was not fully read");
                for (size_t i = 0; i < pattern.bytes.size(); ++i)
                    if (pattern.bytes[i] >= 0 && bytes[i] != pattern.bytes[i])
                        throw std::runtime_error("Referenced helper instructions changed");
                field.evidence = reference->evidence;
                extract(field, spec, Hits{{target}, pattern.bytes.size(), false}, image, decoder);
                field.status = complete && reference->status == Status::Located ? Status::Located : Status::Candidate;
                field.note = "Extracted from the referenced helper with instruction guards; behavior not tested";
            } catch (const std::exception& error) {
                field.status = Status::Unsupported; field.value.reset(); field.text.clear(); field.note = error.what();
            }
        } else {
            const auto it = matches.find(json::text(spec, "rule"));
            if (it == matches.end() || it->second.rvas.empty()) field.note = stop.stop_requested() || image.cancelled ? "已取消；该项未完成" : "当前字节中未找到该方法";
            else if (it->second.rvas.size() > 1 || it->second.capped) { field.status = Status::Ambiguous; field.note = "找到多个代码窗口，未选择其中一个"; }
            else {
                try {
                    extract(field, spec, it->second, image, decoder);
                    field.status = complete ? Status::Located : Status::Candidate;
                    field.note = complete ? "代码定位通过；未进行游戏行为验证" : "命中窗口完整；缺页或取消导致全局唯一性未确认";
                } catch (const std::exception& error) { field.status = Status::Unsupported; field.value.reset(); field.text.clear(); field.note = error.what(); }
            }
        }
        result.fields.push_back(std::move(field));
    }
    if (profile.HasMember("constraints")) for (const auto& constraint : profile["constraints"].GetArray()) {
        const auto a = matches.find(json::text(constraint, "left_rule")), b = matches.find(json::text(constraint, "right_rule"));
        bool okay = a != matches.end() && b != matches.end() && a->second.rvas.size() == 1 && b->second.rvas.size() == 1 &&
            b->second.rvas[0] >= a->second.rvas[0] && b->second.rvas[0] - a->second.rvas[0] == json::number(constraint, "distance");
        if (!okay) for (auto& field : result.fields) if (field.id == json::text(constraint, "field")) {
            field.status = Status::Unsupported; field.value.reset(); field.text.clear(); field.note = "方法各代码段的相对位置不一致";
        }
    }
    const auto& recipes = json::get(profile, "recipes");
    if (!recipes.IsArray() || recipes.Size() > 64) throw std::runtime_error("规则解码列表无效");
    for (const auto& spec : recipes.GetArray()) {
        Recipe recipe;
        recipe.id = json::text(spec, "id"); recipe.name = json::text(spec, "name", recipe.id); recipe.status = Status::Located;
        recipe.body = substitute(json::text(spec, "body"), result, recipe.dependencies, recipe.status);
        if (spec.HasMember("requires")) for (const auto& value : json::array(spec, "requires").GetArray()) {
            if (!value.IsString()) throw std::runtime_error("解码依赖名称无效");
            const std::string id = value.GetString();
            recipe.dependencies.push_back(id);
            const auto* field = result.field(id);
            if (!field || (field->status != Status::Located && field->status != Status::Candidate)) recipe.status = Status::Unsupported;
            else if (field->status == Status::Candidate && recipe.status == Status::Located) recipe.status = Status::Candidate;
        }
        if (spec.HasMember("checks")) for (const auto& check : json::array(spec, "checks").GetArray()) {
            if (check.HasMember("sum")) {
                uint64_t total{};
                bool valid = true;
                for (const auto& id : json::array(check, "sum").GetArray()) {
                    if (!id.IsString()) throw std::runtime_error("解码参数名称无效");
                    const auto* field = result.field(id.GetString());
                    if (!field || !field->value || *field->value > 64 || total > UINT64_MAX - *field->value) valid = false;
                    else total += *field->value;
                }
                if (!valid || total != json::number(check, "equals")) recipe.status = Status::Unsupported;
            }
            if (check.HasMember("same")) {
                std::optional<uint64_t> expected;
                for (const auto& id : json::array(check, "same").GetArray()) {
                    if (!id.IsString()) throw std::runtime_error("交叉引用名称无效");
                    const auto* field = result.field(id.GetString());
                    if (!field || !field->value) recipe.status = Status::Unsupported;
                    else if (expected && *expected != *field->value) recipe.status = Status::Unsupported;
                    else expected = field->value;
                }
            }
        }
        if (recipe.status == Status::Unsupported) recipe.body.clear();
        recipe.note = recipe.body.empty() ? "依赖字段未完整定位，未输出旧公式" : "按本次指令参数展开；静态方法，行为未验证";
        result.recipes.push_back(std::move(recipe));
    }
    result.cancelled = stop.stop_requested() || image.cancelled;
    if (profile.HasMember("unresolved")) for (const auto& note : json::array(profile, "unresolved").GetArray()) {
        if (!note.IsString()) throw std::runtime_error("未解决项格式无效");
        result.notes.emplace_back(note.GetString());
    }
    if (!result.code_total) result.notes.push_back("输入不包含可执行节");
    else if (!result.code_valid) result.notes.push_back("可执行节没有有效字节");
    if (!complete && result.code_valid) result.notes.push_back("代码覆盖不完整；局部命中不等于全局唯一");
    if (result.located_count() == 0 && complete) result.notes.push_back("当前输入未匹配已支持的方法，未回填任何旧值");
    if (progress) progress(1.f, result.cancelled ? "已取消，保留部分结果" : "扫描完成");
    return result;
}
}
