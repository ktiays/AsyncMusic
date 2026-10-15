//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

// Annotations that let Swift import the core idiomatically. They compile away on toolchains
// without Swift, so the headers stay plain C++17 for every other platform.
#if defined(__has_include)
#if __has_include(<swift/bridging>)
#include <swift/bridging>
#endif
#endif

#ifndef SWIFT_SHARED_REFERENCE
#define SWIFT_SHARED_REFERENCE(retain, release)
#endif
#ifndef SWIFT_RETURNS_RETAINED
#define SWIFT_RETURNS_RETAINED
#endif

// Clang nullability, used so Swift sees non-optional pointers. Other compilers ignore it.
#if defined(__clang__)
#define AM_ASSUME_NONNULL_BEGIN                                       \
    _Pragma("clang diagnostic push")                                  \
    _Pragma("clang diagnostic ignored \"-Wnullability-extension\"") \
    _Pragma("clang assume_nonnull begin")
#define AM_ASSUME_NONNULL_END            \
    _Pragma("clang assume_nonnull end") \
    _Pragma("clang diagnostic pop")
#define AM_NULLABLE _Nullable
#define AM_CLOSED_ENUM __attribute__((enum_extensibility(closed)))
#else
#define AM_ASSUME_NONNULL_BEGIN
#define AM_ASSUME_NONNULL_END
#define AM_NULLABLE
#define AM_CLOSED_ENUM
#endif
