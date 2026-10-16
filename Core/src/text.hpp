//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace asyncmusic {

/// The longest prefix of UTF-8 `text` that fits in `maximumBytes` without splitting a character.
inline std::string utf8Prefix(const std::string& text, size_t maximumBytes) {
    if (text.size() <= maximumBytes) return text;
    size_t end = maximumBytes;
    // Back up over continuation bytes (10xxxxxx) to the start of the cut character.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

/// printf-style formatting into a std::string.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string format(const char* pattern, ...) {
    va_list arguments;
    va_start(arguments, pattern);
    va_list copy;
    va_copy(copy, arguments);
    const int length = std::vsnprintf(nullptr, 0, pattern, copy);
    va_end(copy);
    std::string result(length > 0 ? static_cast<size_t>(length) : 0, '\0');
    if (length > 0) std::vsnprintf(result.data(), result.size() + 1, pattern, arguments);
    va_end(arguments);
    return result;
}

}  // namespace asyncmusic
