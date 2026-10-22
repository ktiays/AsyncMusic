//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdint>
#include <string>

namespace asyncmusic::android {

/// Writes JSON text, just enough of it for handing snapshots to the Kotlin side. Commas are
/// placed automatically; keys are given with `key` before each value of an object.
class JsonWriter {
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();
    JsonWriter& key(const char* name);

    JsonWriter& value(const std::string& text);
    JsonWriter& value(const char* text);
    JsonWriter& value(bool flag);
    JsonWriter& value(int number);
    JsonWriter& value(int64_t number);
    JsonWriter& value(double number);
    JsonWriter& null();
    /// A 64-bit ID as 16 hex digits (JSON numbers cannot hold every `uint64_t`).
    JsonWriter& id(uint64_t value);

    const std::string& text() const { return text_; }

private:
    void separate();

    std::string text_;
    /// Whether the next value in the current container needs a comma before it.
    bool needsComma_ = false;
    bool afterKey_ = false;
};

}  // namespace asyncmusic::android
