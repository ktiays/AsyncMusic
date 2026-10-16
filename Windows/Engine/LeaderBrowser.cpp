//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "LeaderBrowser.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <memory>
#include <vector>

#include <asyncmusic/clock_sync.hpp>

#include "Text.h"

#pragma comment(lib, "dnsapi.lib")

namespace asyncmusic::windows {

namespace {

std::wstring serviceDomain() {
    return toUTF16(kClockSyncServiceType) + L".local";
}

/// The instance label of a service instance name, with DNS escapes (`\.`, `\\`, `\DDD`) undone.
std::string instanceLabel(const std::wstring& instance) {
    const std::wstring suffix = L"." + serviceDomain();
    std::wstring label = instance;
    if (label.size() > suffix.size() && _wcsicmp(label.c_str() + label.size() - suffix.size(), suffix.c_str()) == 0) {
        label.resize(label.size() - suffix.size());
    }
    std::string escaped = toUTF8(label);
    std::string result;
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 3 < escaped.size() && isdigit(static_cast<unsigned char>(escaped[i + 1])) && isdigit(static_cast<unsigned char>(escaped[i + 2]))
            && isdigit(static_cast<unsigned char>(escaped[i + 3]))) {
            result.push_back(static_cast<char>((escaped[i + 1] - '0') * 100 + (escaped[i + 2] - '0') * 10 + (escaped[i + 3] - '0')));
            i += 3;
        } else if (escaped[i] == '\\' && i + 1 < escaped.size()) {
            result.push_back(escaped[++i]);
        } else {
            result.push_back(escaped[i]);
        }
    }
    return result;
}

}  // namespace

/// One running browse. The system may still call back after it was cancelled, so this outlives
/// the browser: `owner` is cleared on stop, and the object itself is not freed.
struct LeaderBrowser::Browse {
    std::wstring query;
    DNS_SERVICE_BROWSE_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
    std::mutex mutex;
    LeaderBrowser* owner = nullptr;
};

LeaderBrowser::LeaderBrowser() = default;

LeaderBrowser::~LeaderBrowser() {
    stop();
}

bool LeaderBrowser::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (browse_ != nullptr) return true;
    auto browse = std::make_unique<Browse>();
    browse->query = serviceDomain();
    browse->request.Version = DNS_QUERY_REQUEST_VERSION1;
    browse->request.InterfaceIndex = 0;
    browse->request.QueryName = browse->query.c_str();
    browse->request.pBrowseCallback = [](DWORD status, PVOID context, PDNS_RECORD records) {
        auto* browse = static_cast<Browse*>(context);
        std::lock_guard<std::mutex> lock(browse->mutex);
        if (browse->owner != nullptr) {
            browse->owner->handle(status, records);
        } else if (records != nullptr) {
            DnsRecordListFree(records, DnsFreeRecordList);
        }
    };
    browse->owner = this;
    browse->request.pQueryContext = browse.get();
    const DNS_STATUS status = DnsServiceBrowse(&browse->request, &browse->cancel);
    if (status != DNS_REQUEST_PENDING) {
        error_ = "Browsing for hosts failed (" + std::to_string(status) + ")";
        return false;
    }
    error_.clear();
    browse_ = browse.release();
    return true;
}

void LeaderBrowser::stop() {
    Browse* browse = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        browse = browse_;
        browse_ = nullptr;
        leaders_.clear();
    }
    if (browse != nullptr) {
        DnsServiceBrowseCancel(&browse->cancel);
        // Waits for a callback in flight; later ones find no owner.
        std::lock_guard<std::mutex> lock(browse->mutex);
        browse->owner = nullptr;
    }
}

bool LeaderBrowser::isBrowsing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return browse_ != nullptr;
}

std::string LeaderBrowser::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::vector<LeaderBrowser::Leader> LeaderBrowser::leaders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Leader> result;
    for (const auto& [instance, leader] : leaders_) result.push_back(leader);
    std::sort(result.begin(), result.end(), [](const Leader& a, const Leader& b) { return a.name < b.name; });
    return result;
}

void LeaderBrowser::handle(unsigned long status, void* records) {
    auto* list = static_cast<PDNS_RECORD>(records);
    bool changed = false;
    if (status == ERROR_SUCCESS) {
        std::lock_guard<std::mutex> lock(mutex_);
        {
            for (PDNS_RECORD record = list; record != nullptr; record = record->pNext) {
                if (record->wType != DNS_TYPE_PTR || record->Data.PTR.pNameHost == nullptr) continue;
                const std::wstring instance = record->Data.PTR.pNameHost;
                if (record->dwTtl == 0) {
                    // A goodbye: the leader stopped advertising.
                    changed = leaders_.erase(instance) > 0 || changed;
                } else if (leaders_.find(instance) == leaders_.end()) {
                    leaders_[instance] = Leader{instanceLabel(instance), instance};
                    changed = true;
                }
            }
        }
    }
    if (list != nullptr) DnsRecordListFree(list, DnsFreeRecordList);
    if (changed && onChange) onChange();
}

std::optional<net::Address> LeaderBrowser::resolve(const std::wstring& instance, int timeoutMilliseconds) {
    net::startup();
    struct Context {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::mutex mutex;
        std::optional<net::Address> address;
        ~Context() { CloseHandle(done); }
    };
    auto context = std::make_shared<Context>();
    // The callback owns one reference, so a late callback after a timeout stays safe.
    auto* callbackReference = new std::shared_ptr<Context>(context);

    DNS_SERVICE_RESOLVE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.InterfaceIndex = 0;
    request.QueryName = const_cast<PWSTR>(instance.c_str());
    request.pQueryContext = callbackReference;
    request.pResolveCompletionCallback = [](DWORD status, PVOID opaque, PDNS_SERVICE_INSTANCE result) {
        std::unique_ptr<std::shared_ptr<Context>> reference(static_cast<std::shared_ptr<Context>*>(opaque));
        Context& context = **reference;
        if (status == ERROR_SUCCESS && result != nullptr) {
            std::optional<net::Address> address;
            if (result->ip4Address != nullptr) {
                sockaddr_in ipv4{};
                ipv4.sin_family = AF_INET;
                ipv4.sin_port = htons(result->wPort);
                std::memcpy(&ipv4.sin_addr, result->ip4Address, sizeof(ipv4.sin_addr));
                address = net::Address::fromSockaddr(&ipv4, sizeof(ipv4));
            } else if (result->ip6Address != nullptr) {
                sockaddr_in6 ipv6{};
                ipv6.sin6_family = AF_INET6;
                ipv6.sin6_port = htons(result->wPort);
                std::memcpy(&ipv6.sin6_addr, result->ip6Address, sizeof(ipv6.sin6_addr));
                ipv6.sin6_scope_id = result->dwInterfaceIndex;
                address = net::Address::fromSockaddr(&ipv6, sizeof(ipv6));
            }
            std::lock_guard<std::mutex> lock(context.mutex);
            context.address = address;
        }
        if (result != nullptr) DnsServiceFreeInstance(result);
        SetEvent(context.done);
    };

    DNS_SERVICE_CANCEL cancel{};
    if (DnsServiceResolve(&request, &cancel) != DNS_REQUEST_PENDING) {
        delete callbackReference;
        return std::nullopt;
    }
    if (WaitForSingleObject(context->done, static_cast<DWORD>(timeoutMilliseconds)) != WAIT_OBJECT_0) {
        DnsServiceResolveCancel(&cancel);
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(context->mutex);
    return context->address;
}

// MARK: - Advertising

/// One registration; outlives a late completion callback the same way a browse does.
struct LeaderAdvertiser::Registration {
    DNS_SERVICE_REGISTER_REQUEST request{};
    PDNS_SERVICE_INSTANCE instance = nullptr;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<DWORD> status{ERROR_IO_PENDING};
};

namespace {

/// A DNS label with `.` and `\` escaped.
std::wstring escapeLabel(const std::wstring& label) {
    std::wstring escaped;
    for (wchar_t c : label) {
        if (c == L'.' || c == L'\\') escaped.push_back(L'\\');
        escaped.push_back(c);
    }
    return escaped;
}

std::wstring localHostName() {
    wchar_t name[256] = {};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size) || size == 0) return L"windows.local";
    return std::wstring(name) + L".local";
}

}  // namespace

LeaderAdvertiser::~LeaderAdvertiser() {
    stop();
}

bool LeaderAdvertiser::advertise(const std::string& name, uint16_t port) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (registration_ != nullptr && name_ == name && port_ == port) return true;
    }
    stop();
    const std::wstring instanceName = escapeLabel(toUTF16(name)) + L"." + serviceDomain();
    // The computer's own name, whose addresses the system publishes. (A separate host name
    // carrying only the LAN address would keep followers off unreachable IPv6 addresses, but
    // the system does not answer address queries for such names.)
    const std::wstring host = localHostName();
    auto* registration = new Registration;
    registration->instance = DnsServiceConstructInstance(instanceName.c_str(), host.c_str(), nullptr, nullptr, port, 0, 0, 0, nullptr, nullptr);
    if (registration->instance == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not describe the service for mDNS";
        delete registration;
        return false;
    }
    registration->request.Version = DNS_QUERY_REQUEST_VERSION1;
    registration->request.InterfaceIndex = 0;
    registration->request.pServiceInstance = registration->instance;
    registration->request.pQueryContext = registration;
    registration->request.pRegisterCompletionCallback = [](DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
        auto* registration = static_cast<Registration*>(context);
        registration->status = status;
        if (instance != nullptr) DnsServiceFreeInstance(instance);
        SetEvent(registration->done);
    };
    const DWORD status = DnsServiceRegister(&registration->request, nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    if (status != DNS_REQUEST_PENDING) {
        error_ = "Advertising on the network failed (" + std::to_string(status) + ")";
        DnsServiceFreeInstance(registration->instance);
        CloseHandle(registration->done);
        delete registration;
        return false;
    }
    error_.clear();
    registration_ = registration;
    name_ = name;
    port_ = port;
    return true;
}

void LeaderAdvertiser::stop() {
    Registration* registration = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registration = registration_;
        registration_ = nullptr;
        port_ = 0;
    }
    if (registration == nullptr) return;
    // Wait for the registration to have completed, then withdraw it (which sends goodbyes).
    WaitForSingleObject(registration->done, 2000);
    if (DnsServiceDeRegister(&registration->request, nullptr) == DNS_REQUEST_PENDING) {
        WaitForSingleObject(registration->done, 2000);
    }
    // The instance and the event stay allocated: a late callback may still refer to them.
}

std::string LeaderAdvertiser::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

}  // namespace asyncmusic::windows
