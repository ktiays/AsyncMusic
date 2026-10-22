# The Android engine (AAudio output, NDK media decoding, the JNI bridge) on top of Core.
cmake_minimum_required(VERSION 3.22.1)
project(AsyncMusicAndroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../Core ${CMAKE_CURRENT_BINARY_DIR}/core)

add_library(asyncmusic SHARED
    AudioOutput.cpp
    Engine.cpp
    Json.cpp
    Jni.cpp
    MediaLoader.cpp
)
target_compile_options(asyncmusic PRIVATE -Wall -Wextra)
target_link_libraries(asyncmusic PRIVATE AsyncMusic::Core aaudio mediandk android log)
# 16 KB pages (Android 15+ devices may use them).
target_link_options(asyncmusic PRIVATE "-Wl,-z,max-page-size=16384")
