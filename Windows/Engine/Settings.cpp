//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "Settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <random>

#include "Text.h"

#pragma comment(lib, "advapi32.lib")

namespace asyncmusic::windows::settings {

namespace {

constexpr const wchar_t* kKey = L"Software\\AsyncMusic";

template <typename Value>
std::optional<Value> read(const wchar_t* name, DWORD type) {
    Value value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, type == REG_QWORD ? RRF_RT_REG_QWORD : RRF_RT_REG_BINARY, nullptr, &value, &size) != ERROR_SUCCESS
        || size != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}

void write(const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
}

std::string computerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 64] = {};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size) && size > 0) return toUTF8(name);
    return "Windows PC";
}

}  // namespace

DeviceIdentity deviceIdentity() {
    uint64_t id = read<uint64_t>(L"DeviceID", REG_QWORD).value_or(0);
    if (id == 0) {
        std::random_device device;
        std::mt19937_64 generator((static_cast<uint64_t>(device()) << 32) ^ device() ^ static_cast<uint64_t>(GetTickCount64()));
        while (id == 0) id = generator();
        write(L"DeviceID", REG_QWORD, &id, sizeof(id));
    }
    return DeviceIdentity{id, customDeviceName().value_or(computerName())};
}

std::optional<std::string> customDeviceName() {
    wchar_t name[256] = {};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"DeviceName", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS || name[0] == 0) {
        return std::nullopt;
    }
    return toUTF8(name);
}

void setDeviceName(const std::string& name) {
    const std::wstring wide = toUTF16(name);
    write(L"DeviceName", REG_SZ, wide.c_str(), static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
}

double manualOffset() {
    return read<double>(L"ManualOffset", REG_BINARY).value_or(0);
}

void setManualOffset(double milliseconds) {
    write(L"ManualOffset", REG_BINARY, &milliseconds, sizeof(milliseconds));
}

namespace {
std::wstring assignmentKey(SpeakerChannel channel) {
    return L"SpeakerChannel" + std::to_wstring(static_cast<int>(channel));
}
}  // namespace

GroupSession::SpeakerSettings speakerSettings() {
    GroupSession::SpeakerSettings settings;
    const uint64_t layout = read<uint64_t>(L"SpeakerLayout", REG_QWORD).value_or(0);
    if (layout <= kSpeakerLayoutMaximum) settings.layout = static_cast<SpeakerLayout>(layout);
    for (uint8_t raw = 1; raw <= kSpeakerChannelMaximum; ++raw) {
        const auto channel = static_cast<SpeakerChannel>(raw);
        const uint64_t device = read<uint64_t>(assignmentKey(channel).c_str(), REG_QWORD).value_or(0);
        if (device != 0) settings.assignments[channel] = device;
    }
    return settings;
}

void setSpeakerSettings(const GroupSession::SpeakerSettings& settings) {
    const uint64_t layout = static_cast<uint64_t>(settings.layout);
    write(L"SpeakerLayout", REG_QWORD, &layout, sizeof(layout));
    for (uint8_t raw = 1; raw <= kSpeakerChannelMaximum; ++raw) {
        const auto channel = static_cast<SpeakerChannel>(raw);
        const auto device = settings.assignments.find(channel);
        // 0 is never a device ID, so it stands for "nobody".
        const uint64_t value = device == settings.assignments.end() ? 0 : device->second;
        write(assignmentKey(channel).c_str(), REG_QWORD, &value, sizeof(value));
    }
}

}  // namespace asyncmusic::windows::settings
