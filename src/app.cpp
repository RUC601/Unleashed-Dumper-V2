#include "app.hpp"
#include "json.hpp"
#include <fstream>

namespace ud {
App::App(fs::path runtime) : runtime_(std::move(runtime)) {
    state_.output=runtime_/"dumps";
    const auto settings=runtime_/"settings.json";
    if(fs::exists(settings)) {
        try { state_.language=json::text(json::load(settings),"language")=="en"?Language::English:Language::Chinese; }
        catch(const std::exception&) { state_.message="Language setting could not be read; using Chinese"; }
    }
}
void App::set_language(Language language) {
    std::scoped_lock lock(mutex_); state_.language=language;
    std::ofstream settings(runtime_/"settings.json",std::ios::binary);
    settings << "{\"language\":\"" << (language==Language::English?"en":"zh") << "\"}\n";
    if(!settings) { state_.error=true; state_.message="Language changed for this session; settings could not be saved"; }
}
App::~App() { worker_.request_stop(); if (worker_.joinable()) worker_.join(); dma_.reset(); }
ViewState App::view() const { std::scoped_lock lock(mutex_); return state_; }
void App::progress(float value, const std::string& text) {
    std::scoped_lock lock(mutex_); state_.progress = value; state_.message = text;
}
void App::set_version(const std::string& version) {
    std::scoped_lock lock(mutex_);
    if (!state_.busy && state_.versions.selected >= 0) state_.versions.entered[state_.versions.selected] = version;
}
void App::select_line(int index) {
    std::scoped_lock lock(mutex_);
    if (!state_.busy && state_.versions.selected != index) {
        state_.versions.select(index); state_.result.reset(); state_.saved.clear();
        state_.message = "已选择 " + state_.versions.region() + "，可修改版本后开始分析";
    }
}
void App::set_output(const fs::path& output) {
    std::scoped_lock lock(mutex_); if (!state_.busy) state_.output = output;
}
void App::fetch_version() {
    task("获取 CN / World / Nexon 最新版本…", [this](std::stop_token stop) {
        auto candidates = latest_versions(stop);
        std::scoped_lock lock(mutex_);
        state_.versions.candidates = std::move(candidates);
        state_.message = "候选已更新，选择本次使用的线路；手填版本已保留";
    });
}
void App::finish(Result result, const std::string& version, const fs::path& output) {
    result.archive_version = version;
    result.archive_version = result_version(result);
    auto completed = std::make_shared<Result>(std::move(result));
    {
        std::scoped_lock lock(mutex_); state_.result = completed; state_.saved.clear();
        for (size_t i = 0; i < state_.versions.candidates.size(); ++i)
            if (state_.versions.candidates[i].region == completed->region && state_.versions.entered[i].empty()) state_.versions.entered[i] = completed->archive_version;
        state_.message = completed->cancelled ? "已取消，保留部分结果" : "分析完成，正在保存";
    }
    if (completed->cancelled) return;
    const auto saved = save_bundle(*completed, output, view().language);
    std::scoped_lock lock(mutex_); state_.saved = saved;
    state_.message = completed->profile == "not connected" ? "本线路提取方法待接入，输入信息已按版本保存" : completed->located_count() ? "分析完成，已按版本保存到本地" : "分析完成，未找到已支持的字段；结果已保存";
}
void App::task(const std::string& message, std::function<void(std::stop_token)> work) {
    {
        std::scoped_lock lock(mutex_);
        if (state_.busy) return;
        state_.busy = true; state_.error = false; state_.progress = 0; state_.message = message;
    }
    worker_ = std::jthread([this, work = std::move(work)](std::stop_token stop) {
        try { work(stop); }
        catch (const std::exception& error) { std::scoped_lock lock(mutex_); state_.error = true; state_.message = error.what(); }
        std::scoped_lock lock(mutex_); state_.busy = false;
    });
}
void App::open(const fs::path& file, bool scan_immediately) {
    const auto current = view();
    if (current.busy) return;
    task("打开输入", [this, file, scan_immediately, region = current.versions.region(), version = current.versions.version(), output = current.output](std::stop_token stop) {
        if (file.extension() == L".json" && json::text(json::load(file), "schema") == "unleashed-dumper-result/2") {
            auto result = std::make_shared<Result>(load_result(file));
            dma_.reset();
            std::scoped_lock lock(mutex_);
            state_.connected = false;
            state_.result = result; state_.message = "已打开保存结果"; state_.can_scan = false;
            for (size_t i = 0; i < state_.versions.candidates.size(); ++i) if (state_.versions.candidates[i].region == result->region) {
                state_.versions.select(static_cast<int>(i)); state_.versions.entered[i] = result_version(*result); break;
            }
            state_.saved = file.parent_path();
            state_.input_name = utf8(file.filename().wstring()); input_.clear(); device_selected_ = false;
            return;
        }
        // Selecting an offline source relinquishes the hardware session. Keeping
        // it alive here would block a second tool while this app only reads files.
        dma_.reset();
        {
            std::scoped_lock lock(mutex_);
            state_.connected = false; device_selected_ = false;
            state_.can_scan = false; input_.clear();
        }
        const bool analyze = scan_immediately && !region.empty();
        if (analyze) {
            auto image = Image::load(file, stop, [this](float p, const std::string& t) { progress(p, t); });
            auto result = scan(image, runtime_ / "rules.json", runtime_, region, stop, [this](float p, const std::string& t) { progress(p, t); });
            finish(std::move(result), version, output);
        } else { std::scoped_lock lock(mutex_); state_.result.reset(); }
        std::scoped_lock lock(mutex_);
        input_ = file; device_selected_ = false; state_.input_name = utf8(file.filename().wstring()); state_.can_scan = true;
        if (!analyze) { state_.saved.clear(); state_.message = "已选择输入，选择线路和版本后开始分析"; }
    });
}
void App::connect() {
    task("连接 DMA，读取目标文件头", [this](std::stop_token stop) {
        auto device = std::make_shared<DmaSession>(runtime_);
        auto target = device->identify();
        if (stop.stop_requested()) { progress(0, "连接已取消"); return; }
        std::scoped_lock lock(mutex_);
        dma_ = std::move(device); device_selected_ = true; input_.clear();
        state_.connected = true; state_.can_scan = true; state_.input_name = "DMA · Overwatch.exe · PID " + std::to_string(target.pid);
        state_.result.reset(); state_.message = "DMA 已连接，点击开始扫描";
    });
}
void App::disconnect() {
    task("断开 DMA", [this](std::stop_token) {
        dma_.reset();
        std::scoped_lock lock(mutex_); state_.connected = false;
        if (device_selected_) { device_selected_ = false; state_.can_scan = false; state_.input_name.clear(); }
        state_.message = "DMA 已断开";
    });
}
void App::run() {
    const auto current = view();
    if (!current.can_scan || current.versions.selected < 0) return;
    task("开始分析", [this, region = current.versions.region(), version = current.versions.version(), output = current.output](std::stop_token stop) {
        const auto update = [this](float p, const std::string& t) { progress(p, t); };
        auto image = device_selected_ ? dma_->capture(stop, update) : Image::load(input_, stop, update);
        finish(scan(image, runtime_ / "rules.json", runtime_, region, stop, update), version, output);
    });
}
void App::cancel() { worker_.request_stop(); progress(view().progress, "正在取消，保留已取得的部分结果"); }
void App::save(const fs::path& parent) {
    const auto result = view().result;
    if (!result) return;
    task("保存结果", [this, result, parent](std::stop_token) {
        const auto directory = save_bundle(*result, parent, view().language);
        { std::scoped_lock lock(mutex_); state_.saved = directory; }
        progress(1.f, "已导出：" + utf8(directory.wstring()));
    });
}

}
