//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "Json.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace asyncmusic::android {

void JsonWriter::separate() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (needsComma_) text_ += ',';
}

JsonWriter& JsonWriter::beginObject() {
    separate();
    text_ += '{';
    needsComma_ = false;
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    text_ += '}';
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    separate();
    text_ += '[';
    needsComma_ = false;
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    text_ += ']';
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::key(const char* name) {
    if (needsComma_) text_ += ',';
    text_ += '"';
    text_ += name;
    text_ += "\":";
    afterKey_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(const std::string& text) {
    separate();
    text_ += '"';
    for (const char c : text) {
        switch (c) {
        case '"': text_ += "\\\""; break;
        case '\\': text_ += "\\\\"; break;
        case '\n': text_ += "\\n"; break;
        case '\r': text_ += "\\r"; break;
        case '\t': text_ += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                text_ += escaped;
            } else {
                text_ += c;
            }
        }
    }
    text_ += '"';
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(const char* text) {
    return value(std::string(text));
}

JsonWriter& JsonWriter::value(bool flag) {
    separate();
    text_ += flag ? "true" : "false";
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(int number) {
    return value(static_cast<int64_t>(number));
}

JsonWriter& JsonWriter::value(int64_t number) {
    separate();
    text_ += std::to_string(number);
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(double number) {
    if (!std::isfinite(number)) return null();
    separate();
    char text[32];
    std::snprintf(text, sizeof(text), "%.17g", number);
    text_ += text;
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::null() {
    separate();
    text_ += "null";
    needsComma_ = true;
    return *this;
}

JsonWriter& JsonWriter::id(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "%016" PRIx64, value);
    return this->value(std::string(text));
}

}  // namespace asyncmusic::android
