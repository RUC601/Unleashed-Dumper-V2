cmake_minimum_required(VERSION 3.25)
project(UnleashedDumperV2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(DUMPER_BUILD_GUI "Build the native desktop application" ON)
if(MSVC)
    add_compile_options(/utf-8 /W4 /MP /permissive-)
endif()

add_library(dumper_core STATIC
    src/image.cpp src/scan.cpp src/report.cpp src/dma.cpp src/version.cpp src/text.cpp)
target_include_directories(dumper_core PUBLIC src)
target_include_directories(dumper_core SYSTEM PRIVATE
    vendor/rapidjson vendor/capstone/include vendor/vmm)
target_compile_definitions(dumper_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(dumper_core PUBLIC bcrypt winhttp)

if(DUMPER_BUILD_GUI)
foreach(required_runtime capstone.dll vmm.dll leechcore.dll FTD3XX.dll vcruntime140.dll)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/${required_runtime}")
        message(FATAL_ERROR "Missing portable runtime: runtime/${required_runtime}")
    endif()
endforeach()
add_library(dumper_imgui STATIC vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp vendor/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(dumper_imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_link_libraries(dumper_imgui PUBLIC d3d11 dxgi dwmapi)

add_executable(UnleashedDumperV2 WIN32 src/main.cpp src/app.cpp src/ui.cpp)
target_include_directories(UnleashedDumperV2 SYSTEM PRIVATE vendor/rapidjson)
target_link_libraries(UnleashedDumperV2 PRIVATE dumper_core dumper_imgui comdlg32 shell32 ole32 dcomp windowscodecs)
add_custom_target(dumper_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "$<TARGET_FILE_DIR:UnleashedDumperV2>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:UnleashedDumperV2>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/rules.json" "$<TARGET_FILE_DIR:UnleashedDumperV2>/rules.json"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:UnleashedDumperV2>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/LICENSE.txt" "$<TARGET_FILE_DIR:UnleashedDumperV2>/licenses/imgui.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rapidjson/license.txt" "$<TARGET_FILE_DIR:UnleashedDumperV2>/licenses/rapidjson.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/vendor/capstone/LICENSE.TXT" "$<TARGET_FILE_DIR:UnleashedDumperV2>/licenses/capstone.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vmm/LICENSE.txt" "$<TARGET_FILE_DIR:UnleashedDumperV2>/licenses/leechcore.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vmm/license_info_all.txt" "$<TARGET_FILE_DIR:UnleashedDumperV2>/licenses/vmm-runtime-info.txt")
add_dependencies(UnleashedDumperV2 dumper_runtime)
endif()
