#pragma once
#include "dumper.hpp"
#include <mutex>
#include <thread>

namespace ud {
struct ViewState {
    bool busy{}, connected{}, can_scan{}, error{};
    Language language{Language::Chinese};
    float progress{};
    std::string message{"打开已有 Dump，或连接 DMA 后开始扫描"}, input_name;
    VersionSelection versions;
    fs::path output, saved;
    std::shared_ptr<const Result> result;
};
class App {
public:
    explicit App(fs::path runtime);
    ~App();
    ViewState view() const;
    void open(const fs::path& file, bool scan_immediately = false);
    void connect();
    void disconnect();
    void run();
    void cancel();
    void save(const fs::path& parent);
    void fetch_version();
    void select_line(int index);
    void set_version(const std::string& version);
    void set_output(const fs::path& output);
    void set_language(Language language);
private:
    void task(const std::string& message, std::function<void(std::stop_token)> work);
    void progress(float value, const std::string& message);
    void finish(Result result, const std::string& version, const fs::path& output);
    mutable std::mutex mutex_;
    ViewState state_;
    fs::path runtime_, input_;
    bool device_selected_{};
    std::shared_ptr<DmaSession> dma_;
    std::jthread worker_;
};
void initialize_theme(float scale);
void draw_ui(App& app, void* window);
void load_ui_assets(void* device, const fs::path& runtime);
void release_ui_assets();
}
