#include "app.hpp"
#include "ui_theme.hpp"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <fstream>
#include <algorithm>
#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
ID3D11Device* device{};
ID3D11DeviceContext* context{};
IDXGISwapChain1* swap_chain{};
ID3D11RenderTargetView* render_target{};
IDCompositionDevice* composition{};
IDCompositionTarget* composition_target{};
IDCompositionVisual* composition_visual{};
ud::App* current_app{};
bool close_after_task{};
float dpi_changed{};
UINT resize_width{}, resize_height{};

void release_target() {
    if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
    if (render_target) { render_target->Release(); render_target = nullptr; }
}
void create_target() {
    ID3D11Texture2D* back_buffer{};
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) throw std::runtime_error("无法创建窗口缓冲区");
    const auto result = device->CreateRenderTargetView(back_buffer, nullptr, &render_target);
    back_buffer->Release();
    if (FAILED(result)) throw std::runtime_error("无法创建窗口渲染目标");
}
void release_graphics() {
    ud::release_ui_assets();
    release_target();
    if (composition_visual) { composition_visual->Release(); composition_visual = nullptr; }
    if (composition_target) { composition_target->Release(); composition_target = nullptr; }
    if (composition) { composition->Release(); composition = nullptr; }
    if (swap_chain) { swap_chain->Release(); swap_chain = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }
}
void create_graphics(HWND window) {
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) throw std::runtime_error("DirectX 11 初始化失败");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    RECT area{}; GetClientRect(window, &area);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = area.right; desc.Height = area.bottom; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForComposition(device, &desc, nullptr, &swap_chain)) ||
        FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&composition))) ||
        FAILED(composition->CreateTargetForHwnd(window, TRUE, &composition_target)) ||
        FAILED(composition->CreateVisual(&composition_visual)) || FAILED(composition_visual->SetContent(swap_chain)) ||
        FAILED(composition_target->SetRoot(composition_visual)) || FAILED(composition->Commit()))
        throw std::runtime_error("无法创建透明原生窗口");
    create_target();
}
LRESULT WINAPI procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    // Alt+Space belongs to the native window menu, not the focused ImGui button.
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wparam == VK_SPACE)
        return DefWindowProcW(window, message, wparam, lparam);
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam) return 0;
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(window, &point);
        RECT area{}; GetClientRect(window, &area);
        const auto scale = GetDpiForWindow(window) / 96.f;
        const int edge = static_cast<int>(6 * scale);
        if (!IsZoomed(window)) {
            const bool left = point.x < edge, right = point.x >= area.right - edge;
            const bool top = point.y < edge, bottom = point.y >= area.bottom - edge;
            if (top && left) return HTTOPLEFT; if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT; if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT; if (right) return HTRIGHT; if (top) return HTTOP; if (bottom) return HTBOTTOM;
        }
        const int language_left=static_cast<int>((ud::theme::sidebar_width+22)*scale);
        if(point.y>=15*scale && point.y<49*scale && point.x>=language_left && point.x<language_left+156*scale) return HTCLIENT;
        if (point.y < ud::theme::header_height * scale && point.x < area.right - 100 * scale) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) { resize_width = LOWORD(lparam); resize_height = HIWORD(lparam); }
        return 0;
    case WM_DPICHANGED: {
        dpi_changed = HIWORD(wparam) / 96.f;
        const auto* rectangle = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rectangle->left, rectangle->top, rectangle->right - rectangle->left, rectangle->bottom - rectangle->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const float scale = GetDpiForWindow(window) / 96.f;
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
        info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
        info->ptMinTrackSize = {std::min(static_cast<LONG>(ud::theme::min_width * scale), info->ptMaxSize.x), std::min(static_cast<LONG>(ud::theme::min_height * scale), info->ptMaxSize.y)};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU && lparam != VK_SPACE) return 0;
        break;
    case WM_CLOSE:
        if (current_app && current_app->view().busy) { current_app->cancel(); close_after_task = true; return 0; }
        DestroyWindow(window); return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void print(const std::string& message, bool error = false) {
    const auto handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE) { DWORD written{}; WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr); }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    bool headless=false;
    try {
        int count{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!argv) return 2;
        for(int i=1;i<count;++i) {
            const std::wstring arg=argv[i];
            if(arg==L"--scan"||arg==L"--dma"||arg==L"--latest"||arg==L"--help") headless=true;
        }
        ud::fs::path scan_path, open_path, output;
        std::string region, version;
        bool dma = false, help = false, latest = false;
        for (int i = 1; i < count; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--scan" && i + 1 < count) scan_path = argv[++i];
            else if (arg == L"--open" && i + 1 < count) open_path = argv[++i];
            else if (arg == L"--out" && i + 1 < count) output = argv[++i];
            else if (arg == L"--region" && i + 1 < count) region = ud::utf8(argv[++i]);
            else if (arg == L"--version" && i + 1 < count) version = ud::utf8(argv[++i]);
            else if (arg == L"--dma") dma = true;
            else if (arg == L"--latest") latest = true;
            else if (arg == L"--help") help = true;
            else { LocalFree(argv); throw std::runtime_error("参数无效。使用 --scan <PE或采集目录> --out <导出位置>，或 --open <文件> 打开界面"); }
        }
        LocalFree(argv);
        if (help) { print("UnleashedDumperV2 --scan <PE|capture> --region CN|WORLD|NEXON --out <directory> [--version <name>]\nUnleashedDumperV2 --open <PE|report.json|result.json> [--region CN|WORLD|NEXON] [--version <name>] [--out <directory>]\nUnleashedDumperV2 --latest\nUnleashedDumperV2 --dma --region CN|WORLD|NEXON --out <directory> [--version <name>]\n"); return 0; }
        if (!region.empty() && region != "CN" && region != "WORLD" && region != "NEXON") throw std::runtime_error("请选择 CN、WORLD 或 NEXON");
        if (region.empty() && !version.empty()) throw std::runtime_error("手填版本需要同时选择 --region CN、WORLD 或 NEXON");
        if (unsigned(!scan_path.empty()) + unsigned(!open_path.empty()) + unsigned(dma) + unsigned(latest) > 1)
            throw std::runtime_error("请选择一种输入：文件分析、打开界面、DMA 或版本查询");
        if (latest) {
            const auto candidates = ud::latest_versions();
            for (const auto& line : candidates) {
                for (const auto& v : line.variants) print(line.region + "\t" + v.version + "\t" + v.services + "\n");
                if (!line.error.empty()) print(line.region + "\t" + line.error + "\n", true);
            }
            return std::all_of(candidates.begin(), candidates.end(), [](const ud::VersionLine& line) { return line.error.empty() && !line.variants.empty(); }) ? 0 : 3;
        }
        const auto runtime = ud::executable_dir();
        if (!scan_path.empty() || dma) {
            if (region.empty()) throw std::runtime_error("使用 --region CN、WORLD 或 NEXON 选择本次线路");
            if (output.empty()) throw std::runtime_error("命令行扫描需要 --out 指定导出位置");
            ud::Result result;
            if (dma) { ud::DmaSession session(runtime); result = ud::scan(session.capture({}, {}), runtime / "rules.json", runtime, region); }
            else result = ud::scan(ud::Image::load(scan_path), runtime / "rules.json", runtime, region);
            result.archive_version = version;
            result.archive_version = ud::result_version(result);
            ud::fs::create_directories(output);
            const auto directory = ud::save_bundle(result, output);
            print(ud::utf8(directory.wstring()) + "\n");
            return result.located_count() ? 0 : 3;
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ImGui_ImplWin32_EnableDpiAwareness();
        WNDCLASSEXW klass{sizeof(WNDCLASSEXW), CS_CLASSDC, procedure, 0, 0, instance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"UnleashedDumperV2", nullptr};
        if (!RegisterClassExW(&klass)) throw std::runtime_error("无法建立原生窗口");
        const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY));
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int width = std::min(static_cast<int>(1280 * scale), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left - 32));
        const int height = std::min(static_cast<int>(820 * scale), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 32));
        const auto window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, klass.lpszClassName, L"Unleashed Dumper V2", WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2, monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
            width, height, nullptr, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("无法建立程序窗口");
        create_graphics(window);
        ud::load_ui_assets(device,runtime);
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        ud::initialize_theme(scale);
        ImGui_ImplWin32_Init(window); ImGui_ImplDX11_Init(device, context);
        {
            ud::App app(runtime); current_app = &app;
            if (!region.empty()) app.select_line(region == "CN" ? 0 : region == "WORLD" ? 1 : 2);
            app.set_version(version);
            if (!output.empty()) app.set_output(output);
            if (!open_path.empty()) app.open(open_path, true);
            ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
            BringWindowToTop(window);
            if(!SetForegroundWindow(window)) {
                FLASHWINFO attention{sizeof(attention),window,FLASHW_TRAY,3,0}; FlashWindowEx(&attention);
            }
            bool finished = false;
            while (!finished) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                    if (message.message == WM_QUIT) finished = true;
                }
                if (finished) break;
                if (close_after_task && !app.view().busy) { PostMessageW(window, WM_CLOSE, 0, 0); continue; }
                if (IsIconic(window)) { Sleep(20); continue; }
                if (resize_width && resize_height) {
                    release_target();
                    if (FAILED(swap_chain->ResizeBuffers(0, resize_width, resize_height, DXGI_FORMAT_UNKNOWN, 0))) throw std::runtime_error("窗口缓冲区无法调整大小");
                    resize_width = resize_height = 0; create_target();
                }
                if (dpi_changed) { ImGui_ImplDX11_InvalidateDeviceObjects(); ud::initialize_theme(dpi_changed); dpi_changed = 0; }
                ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
                ud::draw_ui(app, window);
                ImGui::Render();
                const float clear[]{0,0,0,0};
                context->OMSetRenderTargets(1, &render_target, nullptr); context->ClearRenderTargetView(render_target, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                const auto status = swap_chain->Present(1, 0);
                if (status == DXGI_STATUS_OCCLUDED) Sleep(20);
                else if (FAILED(status)) throw std::runtime_error("DirectX 显示设备不可用，请重新打开程序");
            }
            current_app = nullptr;
        }
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        release_graphics(); UnregisterClassW(klass.lpszClassName, instance); CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        current_app = nullptr;
        release_graphics();
        print(std::string(error.what()) + "\n", true);
        if (!headless)
            MessageBoxW(nullptr, ud::wide(error.what()).c_str(), L"Unleashed Dumper V2", MB_OK | MB_ICONERROR);
        return 2;
    }
}
