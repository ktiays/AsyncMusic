//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <string>

namespace asyncmusic::windows {

/// UTF-16 (Windows APIs) to UTF-8 (everything else) and back.
std::string toUTF8(const std::wstring& text);
std::string toUTF8(const wchar_t* text);
std::wstring toUTF16(const std::string& text);

}  // namespace asyncmusic::windows
