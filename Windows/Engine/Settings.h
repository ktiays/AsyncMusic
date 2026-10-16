//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/clock_sync.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace asyncmusic::windows {

/// Per-user settings, kept in the registry under `HKCU\Software\AsyncMusic`.
namespace settings {

/// This device's stable ID (created on first use) and the name others see: the one set with
/// `setDeviceName`, or the computer's name.
DeviceIdentity deviceIdentity();
/// The name set by the user, if any.
std::optional<std::string> customDeviceName();
/// Pass an empty name to go back to the computer's name.
void setDeviceName(const std::string& name);

double manualOffset();
void setManualOffset(double milliseconds);

}  // namespace settings

}  // namespace asyncmusic::windows
