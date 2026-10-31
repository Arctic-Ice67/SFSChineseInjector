cmake_minimum_required(VERSION 3.20)
project(SFSHanInjector CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------- core engine (shared by CLI and GUI) ----------
add_library(han_core STATIC
    src/text.cpp
    src/sha256.cpp
    src/json.cpp
    src/fileio.cpp
    src/clr_image.cpp
    src/patch.cpp
    src/ini.cpp
    src/game.cpp
    src/inject.cpp
    src/inject_flow.cpp
    src/payload.cpp
    src/app.cpp
    src/catalog.cpp
    src/fontcheck.cpp)

target_include_directories(han_core PUBLIC src)
target_compile_options(han_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------- CLI (self test / batch) ----------
add_executable(sfs_han_cli src/cli_main.cpp)
target_link_libraries(sfs_han_cli PRIVATE han_core)

# ---------- GUI (ImGui + DX11 + liquid glass) ----------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_main.cpp)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_executable(SFSChineseInjector WIN32
        src/ui_main.cpp
        src/ui_theme.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(SFSChineseInjector PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends src)
    target_link_libraries(SFSChineseInjector PRIVATE han_core d3d11 dxgi d3dcompiler imm32 ole32 shell32
        comdlg32 dwmapi uuid ws2_32 windowscodecs)
    target_compile_options(SFSChineseInjector PRIVATE -mwindows -static-libgcc -static-libstdc++)
endif()
