#include "dumper.hpp"
#include "json.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ud {
namespace {
template<class T> T integer(std::span<const uint8_t> data, size_t offset) {
    if (offset > data.size() || sizeof(T) > data.size() - offset) throw std::runtime_error("PE 文件头或数据范围不完整");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}
void load_spans(Section& section, const json::Value& rows) {
    if (!rows.IsArray()) throw std::runtime_error("采集有效范围应为数组");
    uint64_t previous{};
    for (const auto& row : rows.GetArray()) {
        if (!row.IsArray() || row.Size() != 2) throw std::runtime_error("采集有效范围格式错误");
        auto begin = json::number(row[0]), size = json::number(row[1]);
        if (!size || begin < previous || begin > section.size || size > section.size - begin)
            throw std::runtime_error("采集有效范围重叠或越界");
        // Adjacent successful reads form one continuous byte range. Keeping a
        // boundary here would make a signature crossing it appear missing.
        if (!section.valid.empty() && begin == previous)
            section.valid.back().size += static_cast<uint32_t>(size);
        else section.valid.push_back({static_cast<uint32_t>(begin), static_cast<uint32_t>(size)});
        previous = begin + size;
    }
}
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("文本编码无效");
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("文本不是有效 UTF-8");
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
fs::path executable_dir() {
    std::vector<wchar_t> path(32768);
    auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("无法定位程序目录");
    return fs::path(std::wstring(path.data(), length)).parent_path();
}
Bytes read_file(const fs::path& path, size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("无法打开文件：" + utf8(path.filename().wstring()));
    const auto length = file.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > limit) throw std::runtime_error("文件大小超出读取范围");
    Bytes bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("文件未完整读取");
    return bytes;
}
std::string sha256(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("无法建立 SHA-256 校验");
    uint8_t digest[32]{};
    auto code = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (code < 0) throw std::runtime_error("文件校验失败");
    std::ostringstream out;
    for (auto b : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
    return out.str();
}
std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << value;
    return out.str();
}
std::string utc_now() {
    SYSTEMTIME value{};
    GetSystemTime(&value);
    char text[32];
    snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute, value.wSecond);
    return text;
}

bool Section::contains(uint64_t address, size_t length) const {
    if (address < rva || address - rva > bytes.size() || length > bytes.size() - static_cast<size_t>(address - rva)) return false;
    const auto offset = address - rva;
    return std::any_of(valid.begin(), valid.end(), [&](Span span) {
        return offset >= span.offset && offset <= uint64_t(span.offset) + span.size && length <= uint64_t(span.offset) + span.size - offset;
    });
}
size_t Section::valid_bytes() const {
    size_t total{};
    for (auto span : valid) total += span.size;
    return total;
}
std::span<const uint8_t> Image::at(uint64_t rva, size_t length) const {
    for (const auto& section : sections)
        if (section.contains(rva, length)) return {section.bytes.data() + static_cast<size_t>(rva - section.rva), length};
    return {};
}
bool Image::code_complete() const {
    bool found = false;
    for (const auto& section : sections) if (section.flags & IMAGE_SCN_MEM_EXECUTE) {
        found = true;
        if (section.valid_bytes() != section.size) return false;
    }
    return found;
}
Image Image::header(std::span<const uint8_t> bytes) {
    if (integer<uint16_t>(bytes, 0) != IMAGE_DOS_SIGNATURE) throw std::runtime_error("请选择 x64 PE Dump、采集目录或保存的结果");
    const auto pe = integer<uint32_t>(bytes, 60);
    if (pe < 64 || pe > 65536 || integer<uint32_t>(bytes, pe) != IMAGE_NT_SIGNATURE) throw std::runtime_error("PE 文件头无效");
    if (integer<uint16_t>(bytes, pe + 4) != IMAGE_FILE_MACHINE_AMD64 || integer<uint16_t>(bytes, pe + 24) != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error("当前支持 Windows x64 PE");
    const auto count = integer<uint16_t>(bytes, pe + 6), optional_size = integer<uint16_t>(bytes, pe + 20);
    if (!count || count > 96 || optional_size < 240) throw std::runtime_error("PE 节表无效");
    Image image;
    image.timestamp = integer<uint32_t>(bytes, pe + 8);
    image.preferred_base = integer<uint64_t>(bytes, pe + 24 + 24);
    image.image_size = integer<uint32_t>(bytes, pe + 24 + 56);
    image.header_size = integer<uint32_t>(bytes, pe + 24 + 60);
    if (image.image_size < 4096 || image.image_size > 512 * 1024 * 1024 || image.header_size > image.image_size)
        throw std::runtime_error("PE 映像大小无效或超出 512 MiB");
    uint64_t allocated{};
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = pe + 24 + optional_size + i * 40;
        if (offset > bytes.size() || 40 > bytes.size() - offset) throw std::runtime_error("PE 节表未完整读取");
        Section section;
        auto name = reinterpret_cast<const char*>(bytes.data() + offset);
        section.name.assign(name, strnlen(name, 8));
        section.rva = integer<uint32_t>(bytes, offset + 12);
        section.raw_size = integer<uint32_t>(bytes, offset + 16);
        section.raw_offset = integer<uint32_t>(bytes, offset + 20);
        section.size = integer<uint32_t>(bytes, offset + 8);
        if (!section.size) section.size = section.raw_size;
        section.flags = integer<uint32_t>(bytes, offset + 36);
        if (section.rva > image.image_size || section.size > image.image_size - section.rva || (section.size && section.rva < image.header_size))
            throw std::runtime_error("PE 节范围超出映像");
        allocated += section.size;
        if (allocated > 512 * 1024 * 1024) throw std::runtime_error("PE 节大小总和超出范围");
        image.sections.push_back(std::move(section));
    }
    for (size_t i = 0; i < image.sections.size(); ++i) for (size_t j = 0; j < i; ++j) {
        const auto& a = image.sections[i]; const auto& b = image.sections[j];
        if (a.size && b.size && a.rva < uint64_t(b.rva) + b.size && b.rva < uint64_t(a.rva) + a.size)
            throw std::runtime_error("PE 节范围重叠");
    }
    return image;
}

void Image::read_version() {
    // Read only stored, valid resource bytes; a profile label is not an observed version.
    std::set<std::string> versions;
    for (const auto& section : sections) if (!(section.flags & IMAGE_SCN_MEM_EXECUTE) && section.name != ".rsrc") for (const auto span : section.valid) {
        const size_t end = static_cast<size_t>(span.offset) + span.size;
        for (size_t offset = span.offset; offset < end; ++offset) {
            if (section.bytes[offset] < '0' || section.bytes[offset] > '9' ||
                (offset > span.offset && ((section.bytes[offset - 1] >= '0' && section.bytes[offset - 1] <= '9') || section.bytes[offset - 1] == '.'))) continue;
            size_t at = offset;
            unsigned components{}, last{};
            bool valid = true;
            while (components < 5 && valid) {
                const auto start = at;
                unsigned value{};
                while (at < end && section.bytes[at] >= '0' && section.bytes[at] <= '9' && at - start < 7) value = value * 10 + section.bytes[at++] - '0';
                if (at == start || (components == 0 && at - start > 2)) valid = false;
                last = value;
                ++components;
                if (components < 5 && valid) { if (at == end || section.bytes[at++] != '.') valid = false; }
            }
            const auto suffix = at;
            while (at < end && at - suffix < 4 && section.bytes[at] >= 'A' && section.bytes[at] <= 'Z') ++at;
            if (valid && components == 5 && last >= 10000 && at < end && section.bytes[at] == 0)
                versions.emplace(reinterpret_cast<const char*>(section.bytes.data() + offset), at - offset);
            if (at > offset) offset = at - 1;
        }
    }
    if (versions.size() == 1) { version = *versions.begin(); version_source = "module version string"; return; }
    if (versions.size() > 1) notes.push_back("multiple module version strings; release version unresolved");
    static const uint8_t key[]{'V',0,'S',0,'_',0,'V',0,'E',0,'R',0,'S',0,'I',0,'O',0,'N',0,'_',0,'I',0,'N',0,'F',0,'O',0,0,0};
    for (const auto& section : sections) if (section.name == ".rsrc") for (const auto span : section.valid) {
        auto begin = section.bytes.begin() + span.offset, end = begin + span.size;
        for (auto pos = std::search(begin, end, std::begin(key), std::end(key)); pos != end; pos = std::search(pos + 1, end, std::begin(key), std::end(key))) {
            const auto start = static_cast<size_t>(pos - section.bytes.begin()) + sizeof(key);
            for (size_t off = start; off + 52 <= uint64_t(span.offset) + span.size && off <= start + 16; ++off) {
                if (integer<uint32_t>(section.bytes, off) != 0xFEEF04BD) continue;
                const auto ms = integer<uint32_t>(section.bytes, off + 8), ls = integer<uint32_t>(section.bytes, off + 12);
                version = std::to_string(ms >> 16) + "." + std::to_string(ms & 65535) + "." + std::to_string(ls >> 16) + "." + std::to_string(ls & 65535);
                version_source = "PE FileVersion";
                return;
            }
        }
    }
}

Image Image::load(const fs::path& input, std::stop_token stop, const Progress& progress) {
    auto path = fs::is_directory(input) ? input / "report.json" : input;
    Image image;
    if (path.extension() == L".json") {
        auto report = json::load(path);
        auto directory = path.parent_path();
        auto header_bytes = read_file(directory / "header.bin", 65536);
        if (sha256(header_bytes) != json::text(json::get(report, "module"), "header_sha256"))
            throw std::runtime_error("采集文件头与报告不匹配");
        image = header(header_bytes);
        const auto status = json::text(report, "status");
        if (!json::flag(report, "identity_stable") || (status != "CAPTURED" && status != "CAPTURED_WITH_GAPS"))
            throw std::runtime_error("请选择已完成且目标身份稳定的代码采集");
        image.source_kind = "captured-image";
        image.region_hint = json::text(report, "region_requested");
        std::set<uint32_t> loaded_sections;
        for (const auto& saved : json::array(json::get(report, "capture"), "sections").GetArray()) {
            if (stop.stop_requested()) { image.cancelled = true; break; }
            const auto name = json::text(saved, "name");
            auto section = std::find_if(image.sections.begin(), image.sections.end(), [&](const Section& s) { return s.name == name; });
            if (section == image.sections.end() || json::number(saved, "rva") != section->rva || json::number(saved, "size") != section->size)
                throw std::runtime_error("采集节与 PE 文件头不匹配");
            if (!loaded_sections.insert(section->rva).second)
                throw std::runtime_error("Capture report contains a duplicate section: " + name);
            const auto file = wide(json::text(saved, "file"));
            if (file.empty() || fs::path(file).filename() != fs::path(file)) throw std::runtime_error("采集节文件名无效");
            section->bytes = read_file(directory / file);
            if (section->bytes.size() != section->size || sha256(section->bytes) != json::text(saved, "sha256_with_gap_placeholders"))
                throw std::runtime_error("采集节长度或摘要不匹配：" + name);
            load_spans(*section, json::get(saved, "valid_spans"));
            if (progress) progress(.12f, "读取采集 " + name);
        }
    } else {
        if (progress) progress(.03f, "读取 PE Dump");
        auto file = read_file(path);
        image = header(file);
        image.source_kind = "pe-file";
        for (auto& section : image.sections) {
            if (stop.stop_requested()) { image.cancelled = true; break; }
            const auto size = std::min(section.size, section.raw_size);
            if (!size) continue;
            if (section.raw_offset > file.size() || size > file.size() - section.raw_offset) throw std::runtime_error("PE 节文件数据被截断：" + section.name);
            section.bytes.assign(file.begin() + section.raw_offset, file.begin() + section.raw_offset + size);
            section.valid.push_back({0, size});
        }
    }
    image.source = fs::absolute(path);
    image.read_version();
    return image;
}
}
