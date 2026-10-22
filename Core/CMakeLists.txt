cmake_minimum_required(VERSION 3.16)
project(AsyncMusicCore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASYNCMUSIC_BUILD_TESTS "Build the core's tests" ${PROJECT_IS_TOP_LEVEL})

# Apple's reference ALAC codec, built as is (its warnings are not ours to fix).
add_library(asyncmusic_alac STATIC
    third_party/alac/ALACBitUtilities.c
    third_party/alac/ALACDecoder.cpp
    third_party/alac/ALACEncoder.cpp
    third_party/alac/EndianPortable.c
    third_party/alac/ag_dec.c
    third_party/alac/ag_enc.c
    third_party/alac/dp_dec.c
    third_party/alac/dp_enc.c
    third_party/alac/matrix_dec.c
    third_party/alac/matrix_enc.c
)
target_include_directories(asyncmusic_alac PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/alac)
if(MSVC)
    target_compile_options(asyncmusic_alac PRIVATE /W0)
    target_compile_definitions(asyncmusic_alac PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(asyncmusic_alac PRIVATE -w)
endif()

add_library(asyncmusic_core
    src/alac.cpp
    src/audio_stream_client.cpp
    src/audio_stream_server.cpp
    src/clock_estimator.cpp
    src/clock_sync_follower.cpp
    src/clock_sync_leader.cpp
    src/host_clock.cpp
    src/media.cpp
    src/net.cpp
    src/playback.cpp
    src/player.cpp
    src/protocol.cpp
    src/renderer.cpp
    src/serial_queue.cpp
    src/session.cpp
    src/speakers.cpp
    src/stream_buffer.cpp
)
add_library(AsyncMusic::Core ALIAS asyncmusic_core)
target_include_directories(asyncmusic_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(asyncmusic_core PRIVATE asyncmusic_alac PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(asyncmusic_core PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(asyncmusic_core PRIVATE /W4)
    # The headers are UTF-8 too, so everything including them needs it.
    target_compile_options(asyncmusic_core PUBLIC /utf-8)
else()
    target_compile_options(asyncmusic_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASYNCMUSIC_BUILD_TESTS)
    enable_testing()
    add_executable(asyncmusic_core_tests tests/core_tests.cpp)
    target_link_libraries(asyncmusic_core_tests PRIVATE asyncmusic_core Threads::Threads)
    add_test(NAME asyncmusic_core_tests COMMAND asyncmusic_core_tests)

    add_executable(test_leader tools/test_leader.cpp)
    target_link_libraries(test_leader PRIVATE asyncmusic_core)
    add_executable(test_host tools/test_host.cpp)
    target_link_libraries(test_host PRIVATE asyncmusic_core)
    add_executable(test_follower tools/test_follower.cpp)
    target_link_libraries(test_follower PRIVATE asyncmusic_core)
endif()
