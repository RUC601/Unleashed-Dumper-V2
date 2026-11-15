#include "app.hpp"
#include "ui_theme.hpp"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <stdexcept>

namespace ud {
namespace {
using namespace theme;
float density = 1.f;
ImFont *regular{}, *bold{}, *mono{}, *brand_font{};
ID3D11ShaderResourceView* brand_logo{};
Language language=Language::Chinese;
const char* tr(const char* zh,const char* en) { return local_text(language,zh,en); }
std::string diagnostic(const std::string& value) { return diagnostic_text(language,value); }
const char* field_status(Status state) {
    switch(state) {
    case Status::Located:return tr("已定位","Located");
    case Status::Candidate:return tr("局部候选","Candidate");
    case Status::Ambiguous:return tr("多个候选","Ambiguous");
    case Status::Missing:return tr("未找到","Missing");
    default:return tr("未支持","Unsupported");
    }
}
ImU32 color(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
void heading(const char* label, float size = 16.f) {
    ImGui::PushFont(bold, size); ImGui::TextUnformatted(label); ImGui::PopFont();
}
void hint(const std::string& label) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)) ImGui::SetTooltip("%s", label.c_str());
}
bool button(const char* label, ImVec2 size = {}, bool primary = false) {
    if(size.x>0) size.x=std::max(size.x,ImGui::CalcTextSize(label,nullptr,true).x+2*ImGui::GetStyle().FramePadding.x);
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.70f,.04f,.19f,1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
    }
    const bool clicked = ImGui::Button(label, size);
    if (primary) ImGui::PopStyleColor(4);
    return clicked;
}
fs::path choose_file(HWND window) {
    wchar_t filename[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window; dialog.lpstrFile = filename; dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
    dialog.lpstrFilter = L"Dump / Capture / V2 Result\0*.exe;*.dll;*.bin;*.json\0All files\0*.*\0";
    const auto title=wide(tr("打开 Dump 或保存结果","Open Dump or saved result")); dialog.lpstrTitle=title.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? fs::path(filename) : fs::path{};
}
fs::path choose_folder(HWND window) {
    IFileOpenDialog* dialog{};
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options{};
    dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); dialog->SetTitle(wide(tr("选择保存位置","Choose output folder")).c_str());
    fs::path path;
    if (SUCCEEDED(dialog->Show(window))) {
        IShellItem* item{};
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR value{};
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) { path = value; CoTaskMemFree(value); }
            item->Release();
        }
    }
    dialog->Release(); return path;
}
void chrome(App& app, HWND window, ImVec2 size) {
    auto* draw = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetWindowPos();
    const float s = density, rail = sidebar_width * s, top = header_height * s;
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + top), color(ImVec4(.065f,.069f,.085f,.98f)), 9*s, ImDrawFlags_RoundCornersTop);
    draw->AddRectFilled(ImVec2(origin.x, origin.y + top), ImVec2(origin.x + rail, origin.y + size.y), color(sidebar), 9*s, ImDrawFlags_RoundCornersBottomLeft);
    draw->AddRectFilled(ImVec2(origin.x + rail, origin.y + top), ImVec2(origin.x + size.x, origin.y + size.y), color(shell), 9*s, ImDrawFlags_RoundCornersBottomRight);
    draw->AddLine(ImVec2(origin.x + rail, origin.y + top), ImVec2(origin.x + rail, origin.y + size.y), color(stroke));
    draw->AddLine(ImVec2(origin.x, origin.y + top), ImVec2(origin.x + size.x, origin.y + top), color(stroke));
    draw->AddRect(origin, ImVec2(origin.x + size.x - 1, origin.y + size.y - 1), color(stroke), 9*s);
    draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(brand_logo)),ImVec2(origin.x+18*s,origin.y+10*s),ImVec2(origin.x+62*s,origin.y+54*s));
    draw->AddText(brand_font,24*s,ImVec2(origin.x+73*s,origin.y+8*s),color(text),"Unleashed");
    draw->AddText(bold,13*s,ImVec2(origin.x+74*s,origin.y+37*s),color(muted),"Dumper V2");
    ImGui::SetCursorPos(ImVec2(rail+22*s,17*s));
    ImGui::PushStyleColor(ImGuiCol_Button,language==Language::Chinese?selected_fill:control);
    if(button("中文##language-zh",ImVec2(64*s,30*s))) app.set_language(Language::Chinese);
    ImGui::PopStyleColor(); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,language==Language::English?selected_fill:control);
    if(button("English##language-en",ImVec2(84*s,30*s))) app.set_language(Language::English);
    ImGui::PopStyleColor();
    const char* names[]{"##minimize-window", "##close-window"};
    for (int i = 0; i < 2; ++i) {
        ImGui::SetCursorPos(ImVec2(size.x - (94 - i*40)*s, 15*s));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        const bool clicked = ImGui::Button(names[i], ImVec2(32*s,32*s));
        ImGui::PopStyleColor(); hint(i ? tr("关闭窗口 · Alt+F4","Close window · Alt+F4") : tr("最小化窗口","Minimize window"));
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImVec2 center{(a.x+b.x)*.5f,(a.y+b.y)*.5f};
        if (i) {
            draw->AddLine(ImVec2(center.x-4*s,center.y-4*s),ImVec2(center.x+4*s,center.y+4*s),color(text),1.5f*s);
            draw->AddLine(ImVec2(center.x+4*s,center.y-4*s),ImVec2(center.x-4*s,center.y+4*s),color(text),1.5f*s);
        } else draw->AddLine(ImVec2(center.x-5*s,center.y),ImVec2(center.x+5*s,center.y),color(text),1.5f*s);
        if (clicked) { if (i) PostMessageW(window, WM_CLOSE, 0, 0); else ShowWindow(window, SW_MINIMIZE); }
    }
}
bool line_item(const char* name, const std::string& version, bool selected, int id, bool compact) {
    const float s = density;
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? selected_fill : ImVec4(.075f,.08f,.10f,.85f));
    const bool clicked = ImGui::Button("##选择线路", ImVec2(-FLT_MIN, (compact?46:56)*s));
    ImGui::PopStyleColor();
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    if (selected) draw->AddRectFilled(ImVec2(a.x,a.y+9*s),ImVec2(a.x+3*s,b.y-9*s),color(accent),2*s);
    draw->AddText(bold,15*s,ImVec2(a.x+14*s,a.y+(compact?6:8)*s),color(text),name);
    draw->AddText(mono,13*s,ImVec2(a.x+14*s,a.y+(compact?26:31)*s),color(muted),version.c_str());
    if (selected) {
        const ImVec2 p{b.x-18*s,(a.y+b.y)*.5f};
        draw->AddLine(ImVec2(p.x-4*s,p.y),ImVec2(p.x-1*s,p.y+3*s),color(text),1.5f*s);
        draw->AddLine(ImVec2(p.x-1*s,p.y+3*s),ImVec2(p.x+5*s,p.y-4*s),color(text),1.5f*s);
    }
    hint(std::string(tr("选择线路：","Select region: ")) + name); ImGui::PopID(); return clicked;
}
void sidebar_view(App& app, const ViewState& state, HWND window, ImVec2 size) {
    const float s = density;
    const bool compact=size.y<730*s;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8*s,(compact?5:7)*s));
    ImGui::SetCursorPos(ImVec2(18*s, (header_height+20)*s));
    ImGui::BeginChild("任务设置", ImVec2((sidebar_width-36)*s, size.y-(header_height+108)*s));
    ImGui::BeginDisabled(state.busy);
    ImGui::AlignTextToFramePadding(); heading(tr("线路","Region")); ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82*s);
    if (button(tr("获取最新","Get latest"),ImVec2(82*s,28*s))) app.fetch_version();
    ImGui::Dummy(ImVec2(0,3*s));
    const char* labels[]{"CN", "World", "Nexon"};
    for (int i = 0; i < 3; ++i) {
        const auto& line = state.versions.candidates[i];
        const auto candidate = line.variants.size()==1 ? line.variants[0].version : line.variants.empty() ? tr("尚未获取候选","Not fetched") : tr("多个候选 · 选择后查看","Multiple versions");
        if (line_item(labels[i],candidate,state.versions.selected==i,i,compact)) app.select_line(i);
    }
    ImGui::Dummy(ImVec2(0,(compact?6:10)*s));
    ImGui::TextDisabled(tr("版本号","Version"));
    const int line = state.versions.selected;
    if (line >= 0) {
        const auto& latest = state.versions.candidates[line];
        if (latest.variants.size() == 1 && latest.variants[0].version != state.versions.version()) {
            ImGui::SameLine(); if (ImGui::SmallButton(tr("采用候选","Use latest"))) app.set_version(latest.variants[0].version);
        } else if (latest.variants.size() > 1) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##线上版本候选", tr("选择线上候选…","Choose a version…"))) {
                for (const auto& candidate : latest.variants)
                    if (ImGui::Selectable((candidate.version+" ("+candidate.services+")").c_str())) app.set_version(candidate.version);
                ImGui::EndCombo();
            }
        }
        if (!latest.error.empty()) ImGui::TextWrapped("%s",diagnostic(latest.error).c_str());
    }
    static char version[128]{};
    const auto current = state.versions.version();
    if (std::string(version)!=current) std::snprintf(version,sizeof(version),"%s",current.c_str());
    ImGui::BeginDisabled(line<0); ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##归档版本号",line<0?tr("先选择线路","Select a region first"):tr("输入或粘贴版本号","Enter or paste a version"),version,sizeof(version))) app.set_version(version);
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0,(compact?10:15)*s)); heading(tr("输入","Input")); ImGui::Dummy(ImVec2(0,3*s));
    const float half = (ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
    if (button(tr("打开文件","Open file"),ImVec2(half,34*s))) { if (auto path=choose_file(window); !path.empty()) app.open(path); }
    ImGui::SameLine();
    if (button(state.connected?tr("断开 DMA","Disconnect DMA"):tr("连接 DMA","Connect DMA"),ImVec2(half,34*s))) { if(state.connected) app.disconnect(); else app.connect(); }
    if (!state.input_name.empty()) ImGui::TextWrapped("%s",state.input_name.c_str());
    else ImGui::TextDisabled(tr("PE Dump / 已有采集 / 保存结果","PE Dump / Capture / Result"));
    ImGui::Dummy(ImVec2(0,(compact?10:15)*s)); heading(tr("本地保存","Local archive")); ImGui::Dummy(ImVec2(0,3*s));
    auto folder=utf8(state.output.filename().wstring()); if(folder.empty()) folder=utf8(state.output.wstring());
    if (button((tr("位置  ·  ","Folder  ·  ")+folder+"##保存位置").c_str(),ImVec2(-FLT_MIN,34*s))) { if(auto path=choose_folder(window);!path.empty()) app.set_output(path); }
    hint(utf8(state.output.wstring()));
    if(!compact) ImGui::TextDisabled(tr("按线路和版本自动收纳","Grouped by region and version"));
    ImGui::EndDisabled(); ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(18*s,size.y-70*s));
    ImGui::BeginDisabled(!state.busy && (!state.can_scan || line<0));
    if(button(state.busy?tr("取消当前任务","Cancel task"):tr("开始分析","Analyze"),ImVec2((sidebar_width-36)*s,40*s),true)) { if(state.busy) app.cancel(); else app.run(); }
    ImGui::EndDisabled();
    ImGui::SetCursorPosX(18*s);
    ImGui::TextDisabled("%s", state.busy?tr("正在处理，可随时取消","Working; cancellation is available"):line<0?tr("选择线路后开始","Select a region to begin"):tr("F5 分析  ·  Ctrl+O 打开","F5 analyze  ·  Ctrl+O open"));
    ImGui::PopStyleVar();
}
const char* group_label(const std::string& group) { return group.c_str(); }
void result_rows(const Result& result,int& selected) {
    const float s=density;
    const float details=selected<0?0:std::min(170*s,ImGui::GetContentRegionAvail().y*.40f);
    ImGui::BeginChild("字段列表",ImVec2(0,details?-details-10*s:0));
    std::vector<std::string> groups;
    for(const auto& f:result.fields) if(!f.hidden && std::find(groups.begin(),groups.end(),f.group)==groups.end()) groups.push_back(f.group);
    for(const auto& group:groups) {
        if(!ImGui::CollapsingHeader(group_label(group),ImGuiTreeNodeFlags_DefaultOpen)) continue;
        if(ImGui::BeginTable(group.c_str(),2,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr("字段","Fields"),ImGuiTableColumnFlags_WidthStretch,1.5f);
            ImGui::TableSetupColumn(tr("值","Value"),ImGuiTableColumnFlags_WidthStretch,1.f);
            for(size_t i=0;i<result.fields.size();++i) {
                const auto& f=result.fields[i]; if(f.hidden || f.group!=group) continue;
                ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(0,29*s); ImGui::TableSetColumnIndex(0);
                if(ImGui::Selectable(f.name.c_str(),selected==static_cast<int>(i),ImGuiSelectableFlags_SpanAllColumns)) selected=static_cast<int>(i);
                hint(diagnostic(f.note)); ImGui::TableSetColumnIndex(1);
                auto value=f.status==Status::Located||f.status==Status::Candidate?result.value_text(f):field_status(f.status);
                if(f.status==Status::Candidate) value=tr("候选 ","Candidate ")+value;
                ImGui::PushFont(mono,14); ImGui::TextColored(f.status==Status::Located?text:warning,"%s",value.c_str()); ImGui::PopFont();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0,6*s));
    }
    if(!result.recipes.empty() && ImGui::CollapsingHeader(tr("解码方法","Decode methods"),ImGuiTreeNodeFlags_DefaultOpen)) for(const auto& r:result.recipes) {
        ImGui::TextUnformatted(r.name.c_str()); ImGui::SameLine(); ImGui::TextDisabled("%s",field_status(r.status));
    }
    if(result.fields.empty()) {
        ImGui::Dummy(ImVec2(0,10*s)); ImGui::TextWrapped(tr("%s 的提取方法尚未接入。","Extraction for %s is not available yet."),result.region.c_str());
        ImGui::TextDisabled(tr("本次已保存输入信息","Input details were archived"));
    }
    ImGui::EndChild();
    if(details) {
        ImGui::Separator(); ImGui::BeginChild("定位依据",ImVec2(0,0));
        const auto& f=result.fields.at(selected); heading(f.name.c_str(),14);
        ImGui::TextWrapped("%s",diagnostic(f.note).c_str());
        if(!f.consumer.empty()) ImGui::TextWrapped("UN: %s",f.consumer.c_str());
        if(ImGui::TreeNode(tr("证据指令","Instructions"))) {
            ImGui::PushFont(mono,13);
            for(const auto& e:f.evidence) ImGui::TextWrapped("%s  %s",hex(e.rva).c_str(),e.instruction.c_str());
            ImGui::PopFont(); ImGui::TreePop();
        }
        ImGui::EndChild();
    }
}
void preview(const std::string& value,const char* id) {
    ImGui::PushFont(mono,14);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.065f,.069f,.085f,.90f));
    ImGui::InputTextMultiline(id,const_cast<char*>(value.c_str()),value.size()+1,ImVec2(-FLT_MIN,-FLT_MIN),ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor(); ImGui::PopFont();
}
bool tab(const char* label,bool selected) {
    ImGui::PushStyleColor(ImGuiCol_Button,selected?selected_fill:ImVec4(0,0,0,0));
    const bool clicked=button(label,ImVec2(0,31*density)); ImGui::PopStyleColor(); return clicked;
}
}


void release_ui_assets() { if(brand_logo) { brand_logo->Release(); brand_logo=nullptr; } }
void load_ui_assets(void* graphics,const fs::path& runtime) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    const auto path=runtime/L"assets/unleashed-logo.png";
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) ||
       FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)) ||
       FAILED(decoder->GetFrame(0,&frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
       FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("Cannot load UN logo: "+utf8(path.wstring()));
    UINT width{},height{}; converter->GetSize(&width,&height);
    if(!width||!height||width>4096||height>4096) throw std::runtime_error("UN logo dimensions are invalid");
    Bytes pixels(static_cast<size_t>(width)*height*4);
    if(FAILED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data()))) throw std::runtime_error("Cannot decode UN logo");
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0}; ComPtr<ID3D11Texture2D> texture;
    auto* device=static_cast<ID3D11Device*>(graphics);
    if(FAILED(device->CreateTexture2D(&desc,&data,&texture)) || FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&brand_logo)))
        throw std::runtime_error("Cannot create UN logo texture");
}

void initialize_theme(float scale) {
    density=scale;
    auto& io=ImGui::GetIO(); io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard; io.IniFilename=nullptr;
    io.Fonts->Clear();
    const auto font=[&](const char* path) {
        auto* face=io.Fonts->AddFontFromFileTTF(path,15);
        if(face) { ImFontConfig merge; merge.MergeMode=true; io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",15,&merge,io.Fonts->GetGlyphRangesChineseFull()); }
        return face;
    };
    regular=font("C:\\Windows\\Fonts\\segoeui.ttf"); bold=font("C:\\Windows\\Fonts\\segoeuib.ttf"); mono=font("C:\\Windows\\Fonts\\consola.ttf");
    brand_font=font("C:\\Windows\\Fonts\\bahnschrift.ttf");
    if(!regular) regular=io.Fonts->AddFontDefault(); if(!bold) bold=regular; if(!mono) mono=regular; if(!brand_font) brand_font=bold;
    io.FontDefault=regular;
    ImGui::StyleColorsDark(); auto& style=ImGui::GetStyle();
    style.WindowRounding=0; style.ChildRounding=6; style.FrameRounding=4; style.PopupRounding=6; style.TabRounding=4;
    style.WindowPadding=ImVec2(16,14); style.FramePadding=ImVec2(10,7); style.ItemSpacing=ImVec2(8,7); style.CellPadding=ImVec2(6,5);
    style.WindowBorderSize=0; style.ChildBorderSize=1; style.FrameBorderSize=1; style.ScrollbarSize=9;
    auto& c=style.Colors;
    c[ImGuiCol_WindowBg]=shell; c[ImGuiCol_ChildBg]=panel; c[ImGuiCol_PopupBg]=ImVec4(.075f,.08f,.10f,1);
    c[ImGuiCol_Text]=text; c[ImGuiCol_TextDisabled]=muted; c[ImGuiCol_Border]=stroke;
    c[ImGuiCol_FrameBg]=control; c[ImGuiCol_FrameBgHovered]=hover; c[ImGuiCol_FrameBgActive]=hover;
    c[ImGuiCol_Button]=control; c[ImGuiCol_ButtonHovered]=hover; c[ImGuiCol_ButtonActive]=selected_fill;
    c[ImGuiCol_Header]=ImVec4(.10f,.105f,.125f,.85f); c[ImGuiCol_HeaderHovered]=hover; c[ImGuiCol_HeaderActive]=selected_fill;
    c[ImGuiCol_CheckMark]=accent; c[ImGuiCol_SliderGrab]=accent; c[ImGuiCol_NavCursor]=focus;
    c[ImGuiCol_TextSelectedBg]=ImVec4(.55f,.04f,.17f,.85f);
    style.ScaleAllSizes(scale); style.FontSizeBase=15; style.FontScaleDpi=scale;
}

void draw_ui(App& app,void* handle) {
    const auto state=app.view(); const auto window=static_cast<HWND>(handle); const float s=density;
    language=state.language;
    const auto* viewport=ImGui::GetMainViewport(); const auto size=viewport->WorkSize;
    ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::Begin("Unleashed Dumper",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(); chrome(app,window,size);
    if(!state.busy && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) { if(auto path=choose_file(window);!path.empty()) app.open(path); }
    if(!state.busy && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5)) app.run();
    sidebar_view(app,state,window,size);
    static const Result* last{}; static int selected=-1,view=0; static bool bbcode{};
    static Language last_language=Language::Chinese;
    static std::string post,engineering,copied;
    const auto& result=state.result;
    if(last!=result.get() || last_language!=language) { last=result.get(); last_language=language; selected=-1; post=result?forum_text(*result,bbcode):""; engineering=result?engineering_text(*result,language):""; copied.clear(); }
    const float x=(sidebar_width+22)*s, width=size.x-x-22*s;
    ImGui::SetCursorPos(ImVec2(x,(header_height+20)*s));
    if(result) {
        heading((result->region+"  /  "+result_version(*result)).c_str(),19);
        ImGui::SetCursorPosX(x);
        const auto methods=static_cast<size_t>(std::count_if(result->recipes.begin(),result->recipes.end(),[](const Recipe& recipe){return recipe.status==Status::Located && !recipe.body.empty();}));
        const std::string coverage=!result->code_total?tr("无可执行节","No executable sections"):
            result->code_valid<result->code_total?std::to_string(result->code_total-result->code_valid)+tr(" 字节代码缺失"," code bytes missing"):
            tr("代码完整","Code complete");
        ImGui::TextDisabled(tr("已定位 %zu 项   ·   方法 %zu/%zu   ·   %s","%zu fields located   ·   methods %zu/%zu   ·   %s"),result->located_count(),methods,result->recipes.size(),coverage.c_str());
    } else { heading(tr("分析结果","Analysis results"),19); ImGui::SetCursorPosX(x); ImGui::TextDisabled(tr("字段、解码方法与可复制的更新参考","Fields, decode methods and copy-ready update references")); }
    const float body_y=(header_height+86)*s, body_h=size.y-body_y-64*s;
    ImGui::SetCursorPos(ImVec2(x,body_y));
    if(!result) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
        ImGui::BeginChild("等待分析",ImVec2(width,body_h));
        ImGui::SetCursorPos(ImVec2(24*s,std::max(40*s,body_h*.30f)));
        heading(state.busy?tr("正在处理…","Working…"):tr("准备开始","Ready to analyze"),22);
        ImGui::SetCursorPosX(24*s); ImGui::TextWrapped(tr("选择左侧线路，打开 Dump 或连接 DMA 后开始分析。","Choose a region, then open a Dump or connect DMA."));
        ImGui::SetCursorPosX(24*s); ImGui::TextWrapped(tr("三服分别提取，支持范围见本次结果；未定位项目保留原因。","Regions have separate extraction rules. Results show coverage and unresolved items."));
        ImGui::EndChild(); ImGui::PopStyleColor();
    } else {
        const bool split=width>=850*s;
        if(split && view==2) view=0;
        const float gap=14*s, left=split?width*.43f:width;
        if(split) {
            ImGui::BeginChild("定位字段面板",ImVec2(left,body_h),ImGuiChildFlags_Borders);
            heading(tr("定位字段","Located fields")); ImGui::Dummy(ImVec2(0,6*s)); result_rows(*result,selected); ImGui::EndChild();
            ImGui::SetCursorPos(ImVec2(x+left+gap,body_y));
        }
        ImGui::BeginChild("正文面板",ImVec2(split?width-left-gap:width,body_h),ImGuiChildFlags_Borders);
        if(!split) { if(tab(tr("字段","Fields"),view==2)) view=2; ImGui::SameLine(); }
        if(tab(tr("帖子文本","Forum text"),view==0)) view=0; ImGui::SameLine(); if(tab(tr("UN 参考","UN reference"),view==1)) view=1;
        ImGui::Dummy(ImVec2(0,4*s));
        if(view==2 && !split) result_rows(*result,selected);
        else preview(view==1?engineering:post,"##结果正文");
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(x,size.y-49*s));
        if(button(view==1?tr("复制参考","Copy reference"):tr("复制帖子","Copy post"),ImVec2(100*s,32*s))) { ImGui::SetClipboardText((view==1?engineering:post).c_str()); copied=view==1?tr("已复制参考","Reference copied"):tr("已复制帖子","Post copied"); }
        ImGui::SameLine(); ImGui::BeginDisabled(state.busy);
        if(button(tr("另存…","Save as…"),ImVec2(76*s,32*s))) { if(auto path=choose_folder(window);!path.empty()) app.save(path); }
        ImGui::EndDisabled();
        if(!state.saved.empty()) { ImGui::SameLine(); if(button(tr("打开目录","Open folder"),ImVec2(90*s,32*s))) ShellExecuteW(window,L"open",state.saved.c_str(),nullptr,nullptr,SW_SHOWNORMAL); hint(utf8(state.saved.wstring())); }
        if(view!=1) { ImGui::SameLine(); if(ImGui::Checkbox("[code]",&bbcode)) post=forum_text(*result,bbcode); }
        if(!copied.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s",copied.c_str()); }
    }
    // A persistent status remains in the sidebar; errors never expire on a timer.
    ImGui::SetCursorPos(ImVec2(x,(header_height+64)*s));
    if(state.busy) { ImGui::PushStyleColor(ImGuiCol_PlotHistogram,accent); ImGui::ProgressBar(state.progress,ImVec2(width,3*s),""); ImGui::PopStyleColor(); }
    ImGui::SetCursorPos(ImVec2(18*s,size.y-91*s));
    const auto status=state.error?tr("出错 · 查看详情","Error · see details"):state.busy?tr("处理中","Working"):state.saved.empty()?tr("就绪","Ready"):tr("已保存到本地","Saved locally");
    ImGui::TextColored(state.error?warning:muted,"%s",status);
    hint(diagnostic(state.message));
    if(state.error || state.busy) { ImGui::SameLine(); if(ImGui::SmallButton(tr("详情","Details"))) ImGui::OpenPopup("任务状态"); }
    if(ImGui::BeginPopup("任务状态")) { ImGui::PushTextWrapPos(ImGui::GetFontSize()*30); ImGui::TextUnformatted(diagnostic(state.message).c_str()); ImGui::PopTextWrapPos(); ImGui::EndPopup(); }
    ImGui::End();
}
}
