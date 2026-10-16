# The Windows engine (WASAPI output, mDNS discovery, settings) on top of Core, and a command
# line tool to run it headless. The WinUI app (AsyncMusic.slnx) compiles the same sources.
cmake_minimum_required(VERSION 3.20)
project(AsyncMusicWindows LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../Core core)

add_library(asyncmusic_windows STATIC
    Engine/AudioOutput.cpp
    Engine/Engine.cpp
    Engine/LeaderBrowser.cpp
    Engine/Settings.cpp
    Engine/Text.cpp
)
target_include_directories(asyncmusic_windows PUBLIC Engine)
target_compile_definitions(asyncmusic_windows PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(asyncmusic_windows PRIVATE /W4 /utf-8)
target_link_libraries(asyncmusic_windows PUBLIC AsyncMusic::Core dnsapi avrt ole32 advapi32)

add_executable(asyncmusic-cli Cli/main.cpp)
target_compile_options(asyncmusic-cli PRIVATE /W4 /utf-8)
target_link_libraries(asyncmusic-cli PRIVATE asyncmusic_windows)
