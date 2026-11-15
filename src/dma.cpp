#include "dumper.hpp"
#include <windows.h>
#include <winternl.h>
#include <vmmdll.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ud {
struct DmaSession::Native {
    HMODULE module{};
    HANDLE mutex{};
    VMM_HANDLE handle{};
    uint32_t pid{};
    uint64_t base{};
    uint64_t process_object{};
    Bytes header;
    decltype(&VMMDLL_Initialize) initialize{};
    decltype(&VMMDLL_Close) close{};
    decltype(&VMMDLL_ConfigSet) configure{};
    decltype(&VMMDLL_PidList) pids{};
    decltype(&VMMDLL_ProcessGetInformation) information{};
    decltype(&VMMDLL_ProcessGetModuleBaseU) module_base{};
    decltype(&VMMDLL_MemReadEx) read_memory{};
    ~Native() {
        if (handle && close) close(handle);
        if (module) FreeLibrary(module);
        if (mutex) CloseHandle(mutex);
    }
    template<class T> T symbol(const char* name) {
        auto value = GetProcAddress(module, name);
        if (!value) throw std::runtime_error(std::string("DMA 库缺少函数：") + name);
        return reinterpret_cast<T>(value);
    }
    bool read(uint32_t process, uint64_t address, std::span<uint8_t> bytes) {
        if (bytes.empty() || bytes.size() > 1024 * 1024 || address < 0x10000 || address >= 0x800000000000 || bytes.size() > 0x800000000000 - address)
            return false;
        DWORD count{};
        constexpr uint64_t flags = VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING |
            VMMDLL_FLAG_NOCACHEPUT | VMMDLL_FLAG_NO_PREDICTIVE_READ;
        return read_memory(handle, process, address, bytes.data(), static_cast<DWORD>(bytes.size()), &count, flags) && count == bytes.size();
    }
    void refresh() {
        // -norefresh disables process and page-table refresh as well as data refresh.
        // NOCACHE alone does not bypass cached virtual-to-physical translations.
        if (!configure(handle, VMMDLL_OPT_REFRESH_ALL, 1))
            throw std::runtime_error("DMA 目标缓存刷新失败；请断开后重新连接");
    }
};

DmaSession::DmaSession(const fs::path& runtime) : native_(std::make_unique<Native>()) {
    auto& n = *native_;
    n.mutex = CreateMutexW(nullptr, FALSE, L"Local\\UnleashedDumperV2_DeviceSession");
    if (!n.mutex || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("另一个 V2 正在使用 DMA；关闭它的连接后重试");
    n.module = LoadLibraryExW((runtime / "vmm.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!n.module) throw std::runtime_error("无法加载 DMA 运行库（Windows error " + std::to_string(GetLastError()) +
        "）；请使用完整运行目录，包含 vmm / leechcore / FTD3XX / vcruntime140");
    n.initialize = n.symbol<decltype(n.initialize)>("VMMDLL_Initialize");
    n.close = n.symbol<decltype(n.close)>("VMMDLL_Close");
    n.configure = n.symbol<decltype(n.configure)>("VMMDLL_ConfigSet");
    n.pids = n.symbol<decltype(n.pids)>("VMMDLL_PidList");
    n.information = n.symbol<decltype(n.information)>("VMMDLL_ProcessGetInformation");
    n.module_base = n.symbol<decltype(n.module_base)>("VMMDLL_ProcessGetModuleBaseU");
    n.read_memory = n.symbol<decltype(n.read_memory)>("VMMDLL_MemReadEx");
    char empty[] = "", device[] = "-device", fpga[] = "fpga", python[] = "-disable-python", symbols[] = "-disable-symbolserver", refresh[] = "-norefresh";
    LPSTR args[]{empty, device, fpga, python, symbols, refresh};
    n.handle = n.initialize(static_cast<DWORD>(std::size(args)), args);
    if (!n.handle) throw std::runtime_error("DMA 连接失败；检查设备、驱动和其他占用程序后重试");
}
DmaSession::~DmaSession() = default;

Image DmaSession::identify() {
    auto& n = *native_;
    n.refresh();
    SIZE_T count{};
    if (!n.pids(n.handle, nullptr, &count) || !count || count > 32768) throw std::runtime_error("无法枚举目标进程");
    std::vector<DWORD> ids(count);
    if (!n.pids(n.handle, ids.data(), &count) || count > ids.size()) throw std::runtime_error("进程列表变化，请重新连接");
    std::vector<VMMDLL_PROCESS_INFORMATION> targets;
    for (size_t i = 0; i < count; ++i) {
        VMMDLL_PROCESS_INFORMATION information{};
        information.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
        information.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
        information.wSize = sizeof(information);
        SIZE_T bytes = sizeof(information);
        if (!n.information(n.handle, ids[i], &information, &bytes)) continue;
        if (information.dwState == 0 && (!_stricmp(information.szName, "Overwatch.exe") || !_stricmp(information.szNameLong, "Overwatch.exe"))) targets.push_back(information);
    }
    if (targets.empty()) throw std::runtime_error("未找到可读取的 Overwatch.exe；启动目标后重新连接");
    if (targets.size() != 1) throw std::runtime_error("找到多个 Overwatch.exe，请先保留一个目标");
    if (targets[0].win.fWow64) throw std::runtime_error("当前只支持 x64 目标");
    n.pid = targets[0].dwPID;
    n.process_object = targets[0].win.vaEPROCESS;
    n.base = n.module_base(n.handle, n.pid, const_cast<LPSTR>("Overwatch.exe"));
    if (!n.base) throw std::runtime_error("无法定位 Overwatch.exe 模块");
    n.header.assign(4096, 0);
    if (!n.read(n.pid, n.base, n.header)) throw std::runtime_error("目标 PE 文件头未完整读取，请重试");
    auto image = Image::header(n.header);
    image.source_kind = "live-dma"; image.source = L"DMA-Overwatch.exe";
    image.pid = n.pid; image.image_base = n.base;
    return image;
}

Image DmaSession::capture(std::stop_token stop, const Progress& progress) {
    auto image = identify();
    auto& n = *native_;
    const auto pid = n.pid;
    const auto base = n.base;
    const auto process_object = n.process_object;
    const auto first_header = n.header;
    uint64_t total{}, finished{};
    for (const auto& section : image.sections) total += section.size;
    for (auto& section : image.sections) {
        if (stop.stop_requested()) { image.cancelled = true; break; }
        section.bytes.resize(section.size);
        auto valid = [&](uint32_t offset, uint32_t size) {
            if (!section.valid.empty() && section.valid.back().offset + section.valid.back().size == offset) section.valid.back().size += size;
            else section.valid.push_back({offset, size});
        };
        for (uint32_t offset = 0; offset < section.size; offset += 65536) {
            if (stop.stop_requested()) { image.cancelled = true; break; }
            const uint32_t size = std::min(65536u, section.size - offset);
            auto bytes = std::span<uint8_t>(section.bytes).subspan(offset, size);
            if (n.read(pid, base + section.rva + offset, bytes)) valid(offset, size);
            else {
                // A failed large read does not make its partial bytes valid; retry complete pages.
                std::fill(bytes.begin(), bytes.end(), 0);
                for (uint32_t page = 0; page < size; page += 4096) {
                    if (stop.stop_requested()) { image.cancelled = true; break; }
                    const auto length = std::min(4096u, size - page);
                    auto chunk = bytes.subspan(page, length);
                    if (n.read(pid, base + section.rva + offset + page, chunk)) valid(offset + page, length);
                    else std::fill(chunk.begin(), chunk.end(), 0);
                }
            }
            finished += size;
            if (progress) progress(.02f + .16f * static_cast<float>(double(finished) / std::max<uint64_t>(1, total)), "DMA 读取 " + section.name);
        }
        if (image.cancelled) break;
    }
    // Refresh and re-enumerate so a restarted target cannot pass using a cached
    // process/module list, including PID reuse with an identical PE header.
    identify();
    if (n.pid != pid || n.process_object != process_object || n.base != base || n.header != first_header)
        throw std::runtime_error("采集期间目标模块变化，本次结果未合并；重新连接后扫描");
    image.read_version();
    return image;
}
}
