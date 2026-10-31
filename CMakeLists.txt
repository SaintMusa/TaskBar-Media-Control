cmake_minimum_required(VERSION 3.25)
project(TaskbarMedia LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: one self-contained exe, no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(TaskbarMedia WIN32
    src/main.cpp
    src/app_filter.cpp
    src/app_info.cpp
    src/hotkey.cpp
    src/list_text.cpp
    src/taskbar_watcher.cpp
    src/media_watcher.cpp
    src/placement.cpp
    src/popup.cpp
    src/settings_window.cpp
    src/settings.cpp
    src/volume.cpp
    src/app.manifest
)

target_compile_definitions(TaskbarMedia PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(TaskbarMedia PRIVATE /W4 /permissive- /utf-8 /bigobj)
# windowsapp: C++/WinRT, for the Windows media session API.
target_link_libraries(TaskbarMedia PRIVATE d2d1 dwrite ole32 oleaut32 uuid windowsapp windowscodecs shcore shell32 version comctl32 dwmapi)

enable_testing()
add_executable(AppFilterTests tests/app_filter_test.cpp src/app_filter.cpp)
target_include_directories(AppFilterTests PRIVATE src)
target_compile_options(AppFilterTests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME AppFilterTests COMMAND AppFilterTests)

add_executable(PlacementTests tests/placement_test.cpp src/placement.cpp)
target_include_directories(PlacementTests PRIVATE src)
target_compile_options(PlacementTests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME PlacementTests COMMAND PlacementTests)

add_executable(HotkeyTests tests/hotkey_test.cpp src/hotkey.cpp)
target_include_directories(HotkeyTests PRIVATE src)
target_compile_options(HotkeyTests PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(HotkeyTests PRIVATE NOMINMAX)
add_test(NAME HotkeyTests COMMAND HotkeyTests)

add_executable(ListTextTests tests/list_text_test.cpp src/list_text.cpp)
target_include_directories(ListTextTests PRIVATE src)
target_compile_options(ListTextTests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME ListTextTests COMMAND ListTextTests)
