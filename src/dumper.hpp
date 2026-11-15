#pragma once
#include "text.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace ud {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Progress = std::function<void(float, const std::string&)>;

struct Span { uint32_t offset{}, size{}; };
struct Section {
    std::string name;
    uint32_t rva{}, size{}, flags{}, raw_offset{}, raw_size{};
    Bytes bytes;
    std::vector<Span> valid;
    bool contains(uint64_t address, size_t length) const;
    size_t valid_bytes() const;
};

struct Image {
    fs::path source;
    std::string source_kind, version, version_source, region_hint;
    uint32_t timestamp{}, image_size{}, header_size{}, pid{};
    uint64_t image_base{}, preferred_base{};
    bool cancelled{};
    std::vector<Section> sections;
    std::vector<std::string> notes;
    static Image load(const fs::path& path, std::stop_token stop = {}, const Progress& progress = {});
    static Image header(std::span<const uint8_t> bytes);
    std::span<const uint8_t> at(uint64_t rva, size_t length) const;
    bool code_complete() const;
    void read_version();
};

enum class Status { Located, Candidate, Ambiguous, Missing, Unsupported };
std::string status_text(Status status);
std::string status_key(Status status);
Status parse_status(const std::string& key);

struct Evidence {
    uint64_t rva{};
    std::string instruction, bytes, rule;
};
struct Field {
    std::string id, name, group, kind, consumer, note;
    std::optional<uint64_t> value, previous;
    std::string text;
    Status status{Status::Missing};
    bool hidden{};
    std::vector<Evidence> evidence;
};
struct Recipe {
    std::string id, name, body, note;
    Status status{Status::Missing};
    std::vector<std::string> dependencies;
};
struct Result {
    std::string region, profile, version, version_source, created_utc, source_kind, source_name;
    std::string archive_version;
    uint32_t timestamp{}, image_size{};
    uint64_t code_valid{}, code_total{};
    bool cancelled{};
    std::vector<Field> fields;
    std::vector<Recipe> recipes;
    std::vector<std::string> notes;
    const Field* field(const std::string& id) const;
    std::string value_text(const Field& field) const;
    size_t located_count() const;
};

Result scan(const Image& image, const fs::path& rules, const fs::path& runtime,
            const std::string& region, std::stop_token stop = {}, const Progress& progress = {});
std::string forum_text(const Result& result, bool bbcode = false);
std::string engineering_text(const Result& result, Language language = Language::Chinese);
std::string reference_header(const Result& result);
std::string result_json(const Result& result);
Result load_result(const fs::path& path);
fs::path save_bundle(const Result& result, const fs::path& parent, Language language = Language::Chinese);
std::string result_version(const Result& result);
struct VersionCandidate { std::string version, build_id, services; };
struct VersionLine { std::string region; std::vector<VersionCandidate> variants; std::string error; };
using VersionCandidates = std::array<VersionLine, 3>;
struct VersionSelection {
    VersionCandidates candidates{{{"CN", {}, {}}, {"WORLD", {}, {}}, {"NEXON", {}, {}}}};
    std::array<std::string, 3> entered;
    int selected{-1};
    void select(int index);
    std::string region() const;
    std::string version() const;
};
VersionCandidate parse_version_table(const std::string& text, const std::string& service = "cn");
VersionCandidates parse_version_candidates(const std::string& text);
VersionCandidates latest_versions(std::stop_token stop = {});
std::string hex(uint64_t value);
std::string utc_now();
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
Bytes read_file(const fs::path& path, size_t limit = 512ull * 1024 * 1024);
std::string sha256(std::span<const uint8_t> bytes);
fs::path executable_dir();

class DmaSession {
public:
    explicit DmaSession(const fs::path& runtime);
    ~DmaSession();
    DmaSession(const DmaSession&) = delete;
    DmaSession& operator=(const DmaSession&) = delete;
    Image identify();
    Image capture(std::stop_token stop, const Progress& progress);
private:
    struct Native;
    std::unique_ptr<Native> native_;
};
}
