cmake_minimum_required(VERSION 3.16)
project(AsyncMusicCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASYNCMUSIC_BUILD_TESTS "Build the core's tests" ${PROJECT_IS_TOP_LEVEL})

add_library(asyncmusic_core
    src/clock_estimator.cpp
    src/host_clock.cpp
    src/playback.cpp
    src/protocol.cpp
    src/renderer.cpp
    src/stream_buffer.cpp
)
add_library(AsyncMusic::Core ALIAS asyncmusic_core)
target_include_directories(asyncmusic_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(asyncmusic_core PRIVATE /W4)
else()
    target_compile_options(asyncmusic_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASYNCMUSIC_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(asyncmusic_core_tests tests/core_tests.cpp)
    target_link_libraries(asyncmusic_core_tests PRIVATE asyncmusic_core Threads::Threads)
    add_test(NAME asyncmusic_core_tests COMMAND asyncmusic_core_tests)
endif()
