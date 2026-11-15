#pragma once
#include <imgui.h>

namespace ud::theme {
inline constexpr float header_height = 64.f, sidebar_width = 276.f;
inline constexpr int min_width = 900, min_height = 620;
inline constexpr ImVec4 shell{18.f/255,19.f/255,23.f/255,.96f};
inline constexpr ImVec4 sidebar{15.f/255,16.f/255,20.f/255,.97f};
inline constexpr ImVec4 panel{22.f/255,23.f/255,28.f/255,.93f};
inline constexpr ImVec4 control{27.f/255,32.f/255,36.f/255,1};
inline constexpr ImVec4 hover{35.f/255,40.f/255,46.f/255,1};
inline constexpr ImVec4 stroke{52.f/255,55.f/255,64.f/255,1};
inline constexpr ImVec4 text{.94f,.95f,.97f,1};
inline constexpr ImVec4 muted{168.f/255,174.f/255,184.f/255,1};
inline constexpr ImVec4 accent{228.f/255,17.f/255,67.f/255,1};
inline constexpr ImVec4 accent_hover{.84f,.05f,.24f,1};
inline constexpr ImVec4 selected_fill{.27f,.075f,.125f,1};
inline constexpr ImVec4 warning{.95f,.77f,.49f,1};
inline constexpr ImVec4 focus{1,.85f,.45f,1};
}
