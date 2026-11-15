#include "dumper.hpp"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace ud {
namespace {
struct Internet {
    HINTERNET handle{};
    ~Internet() { if (handle) WinHttpCloseHandle(handle); }
};
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> values;
    size_t start = 0;
    do {
        const auto end = line.find('|', start);
        values.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    return values;
}
void requested(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("已取消版本查询，可直接手动输入");
}
}

VersionCandidate parse_version_table(const std::string& text, const std::string& service) {
    std::istringstream input(text);
    std::vector<std::string> columns;
    std::string line;
    VersionCandidate candidate;
    for (size_t lines = 0; std::getline(input, line); ++lines) {
        if (lines >= 256 || line.size() > 4096) throw std::runtime_error("版本表格式异常，可手动输入版本号");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        auto row = split(line);
        if (columns.empty()) {
            for (auto& name : row) name = name.substr(0, name.find('!'));
            columns = std::move(row);
            if (std::count(columns.begin(), columns.end(), "Region") != 1 || std::count(columns.begin(), columns.end(), "VersionsName") != 1)
                throw std::runtime_error("版本表缺少版本列，可手动输入版本号");
            continue;
        }
        if (row.size() != columns.size()) throw std::runtime_error("版本表行不完整，可手动输入版本号");
        const auto at = [&](const std::string& name) -> std::string {
            const auto p = std::find(columns.begin(), columns.end(), name);
            return p == columns.end() ? "" : row[static_cast<size_t>(p - columns.begin())];
        };
        if (at("Region") != service) continue;
        if (!candidate.version.empty()) throw std::runtime_error("版本表中 " + service + " 有多条记录，可手动输入");
        candidate = {at("VersionsName"), at("BuildId"), service};
        if (candidate.version.empty() || candidate.version.size() > 96 ||
            !std::all_of(candidate.version.begin(), candidate.version.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '.'; }))
            throw std::runtime_error("线上版本号格式异常，可手动输入版本号");
    }
    if (candidate.version.empty()) throw std::runtime_error("线上版本表没有 " + service + " 项，可直接手动输入");
    return candidate;
}

VersionCandidates parse_version_candidates(const std::string& text) {
    VersionCandidates lines{{{"CN", {}, {}}, {"WORLD", {}, {}}, {"NEXON", {}, {}}}};
    const std::array<std::vector<std::string>, 3> services{{{"cn"}, {"us", "eu", "kr"}, {"nexon"}}};
    for (size_t i = 0; i < lines.size(); ++i) for (const auto& service : services[i]) {
        try {
            auto candidate = parse_version_table(text, service);
            auto same = std::find_if(lines[i].variants.begin(), lines[i].variants.end(), [&](const VersionCandidate& v) { return v.version == candidate.version; });
            if (same == lines[i].variants.end()) lines[i].variants.push_back(std::move(candidate));
            else same->services += "/" + service;
        } catch (const std::exception& e) {
            if (!lines[i].error.empty()) lines[i].error += "；";
            lines[i].error += e.what();
        }
    }
    return lines;
}

void VersionSelection::select(int index) {
    if (index < 0 || index >= static_cast<int>(candidates.size())) throw std::runtime_error("请选择 CN、World 或 Nexon");
    selected = index;
    if (entered[index].empty() && candidates[index].variants.size() == 1) entered[index] = candidates[index].variants[0].version;
}
std::string VersionSelection::region() const { return selected < 0 ? "" : candidates.at(selected).region; }
std::string VersionSelection::version() const { return selected < 0 ? "" : entered.at(selected); }

VersionCandidates latest_versions(std::stop_token stop) {
    requested(stop);
    Internet session{WinHttpOpen(L"Unleashed-Dumper-V2/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle) throw std::runtime_error("无法发起版本查询，可手动输入");
    WinHttpSetTimeouts(session.handle, 3000, 4000, 4000, 4000);
    // Same public provider as the existing version component; no Python process or local service.
    Internet connection{WinHttpConnect(session.handle, L"us.version.battle.net", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.handle) throw std::runtime_error("无法连接版本服务，可手动输入");
    Internet request{WinHttpOpenRequest(connection.handle, L"GET", L"/v2/products/pro/versions", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.handle) throw std::runtime_error("无法建立版本请求，可手动输入");
    if (!WinHttpSendRequest(request.handle, L"Accept: text/plain\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.handle, nullptr))
        throw std::runtime_error("版本查询失败或超时，可直接手动输入");
    requested(stop);
    DWORD status{}, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("版本服务暂不可用，可直接手动输入");
    std::string body;
    const auto deadline = GetTickCount64() + 8000;
    while (true) {
        requested(stop);
        DWORD count{};
        char buffer[4096];
        if (GetTickCount64() > deadline || !WinHttpReadData(request.handle, buffer, sizeof(buffer), &count))
            throw std::runtime_error("版本查询超时，可直接手动输入");
        if (!count) break;
        body.append(buffer, count);
        if (body.size() > 65536) throw std::runtime_error("版本响应过大，可手动输入版本号");
    }
    return parse_version_candidates(body);
}
}
