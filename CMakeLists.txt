cmake_minimum_required(VERSION 3.25)
project(TaskbarMedia LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: one self-contained exe, no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(TaskbarMedia WIN32
    src/main.cpp
    src/app_info.cpp
    src/taskbar_watcher.cpp
    src/media_watcher.cpp
    src/app.manifest
)

target_compile_definitions(TaskbarMedia PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(TaskbarMedia PRIVATE /W4 /permissive- /utf-8 /bigobj)
# windowsapp: C++/WinRT, for the Windows media session API.
target_link_libraries(TaskbarMedia PRIVATE d2d1 dwrite ole32 oleaut32 uuid windowsapp windowscodecs shcore shell32 version)
