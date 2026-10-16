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

}  // namespace asyncmusic::windows::settings
