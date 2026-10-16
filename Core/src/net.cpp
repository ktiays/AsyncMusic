//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/net.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace asyncmusic::net {

namespace {

#if defined(_WIN32)
using Native = SOCKET;
constexpr Native kInvalid = INVALID_SOCKET;
int lastError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
int pollSockets(WSAPOLLFD* descriptors, ULONG count, int timeout) { return WSAPoll(descriptors, count, timeout); }
using PollDescriptor = WSAPOLLFD;
using Length = int;
#else
using Native = int;
constexpr Native kInvalid = -1;
int lastError() { return errno; }
bool wouldBlock(int error) { return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN; }
int pollSockets(pollfd* descriptors, nfds_t count, int timeout) { return ::poll(descriptors, count, timeout); }
using PollDescriptor = pollfd;
using Length = socklen_t;
#endif

Native native(intptr_t handle) { return static_cast<Native>(handle); }
intptr_t wrap(Native socket) { return static_cast<intptr_t>(socket); }

void closeNative(Native socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

void setNonBlocking(Native socket, bool enabled) {
#if defined(_WIN32)
    u_long mode = enabled ? 1 : 0;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

/// 1 ready, 0 timeout, -1 error.
int waitFor(Native socket, short events, int timeoutMilliseconds) {
    PollDescriptor descriptor{};
    descriptor.fd = socket;
    descriptor.events = events;
    const int result = pollSockets(&descriptor, 1, timeoutMilliseconds);
    if (result < 0) {
#if !defined(_WIN32)
        if (errno == EINTR) return 0;
#endif
        return -1;
    }
    return result == 0 ? 0 : 1;
}

}  // namespace

bool startup() {
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
#else
    return true;
#endif
}

std::string lastErrorDescription() {
    const int error = lastError();
#if defined(_WIN32)
    char* message = nullptr;
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(error), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<char*>(&message), 0, nullptr);
    std::string text = length > 0 && message != nullptr ? std::string(message, length) : "Socket error";
    if (message != nullptr) LocalFree(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) text.pop_back();
    return text + " (" + std::to_string(error) + ")";
#else
    return std::string(std::strerror(error)) + " (" + std::to_string(error) + ")";
#endif
}

// MARK: - Address

std::optional<Address> Address::fromString(const std::string& host, uint16_t port) {
    startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return std::nullopt;
    }
    Address address = fromSockaddr(result->ai_addr, static_cast<size_t>(result->ai_addrlen));
    freeaddrinfo(result);
    if (!address.isValid()) return std::nullopt;
    return address.withPort(port);
}

Address Address::fromSockaddr(const void* address, size_t length) {
    Address result;
    if (address == nullptr || length > sizeof(result.storage_)) return result;
    const auto family = static_cast<const ::sockaddr*>(address)->sa_family;
    if ((family == AF_INET && length >= sizeof(sockaddr_in)) || (family == AF_INET6 && length >= sizeof(sockaddr_in6))) {
        std::memcpy(result.storage_, address, length);
        result.length_ = static_cast<uint32_t>(family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
    }
    return result;
}

int Address::family() const {
    return isValid() ? reinterpret_cast<const ::sockaddr*>(storage_)->sa_family : AF_UNSPEC;
}

uint16_t Address::port() const {
    switch (family()) {
    case AF_INET: return ntohs(reinterpret_cast<const sockaddr_in*>(storage_)->sin_port);
    case AF_INET6: return ntohs(reinterpret_cast<const sockaddr_in6*>(storage_)->sin6_port);
    default: return 0;
    }
}

Address Address::withPort(uint16_t port) const {
    Address result = *this;
    switch (family()) {
    case AF_INET: reinterpret_cast<sockaddr_in*>(result.storage_)->sin_port = htons(port); break;
    case AF_INET6: reinterpret_cast<sockaddr_in6*>(result.storage_)->sin6_port = htons(port); break;
    default: break;
    }
    return result;
}

std::string Address::host() const {
    char text[INET6_ADDRSTRLEN + 16] = {};
    if (!isValid() || getnameinfo(reinterpret_cast<const ::sockaddr*>(storage_), static_cast<Length>(length_), text, sizeof(text), nullptr, 0, NI_NUMERICHOST) != 0) {
        return "?";
    }
    return text;
}

std::string Address::toString() const {
    const std::string name = host();
    return (family() == AF_INET6 ? "[" + name + "]" : name) + ":" + std::to_string(port());
}

bool Address::operator==(const Address& other) const {
    return length_ == other.length_ && std::memcmp(storage_, other.storage_, length_) == 0;
}

// MARK: - Socket

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = wrap(kInvalid);
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = wrap(kInvalid);
    }
    return *this;
}

bool Socket::isOpen() const {
    return native(handle_) != kInvalid;
}

void Socket::close() {
    if (isOpen()) {
        closeNative(native(handle_));
        handle_ = wrap(kInvalid);
    }
}

bool Socket::create(int family, int type, int protocol) {
    close();
    startup();
    const Native socket = ::socket(family, type, protocol);
    if (socket == kInvalid) return false;
    handle_ = wrap(socket);
#if defined(__APPLE__)
    // Writing to a closed connection must fail, not kill the process.
    const int one = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return true;
}

int Socket::waitReadable(int timeoutMilliseconds) const {
    if (!isOpen()) return -1;
    return waitFor(native(handle_), POLLIN, timeoutMilliseconds);
}

Address Socket::localAddress() const {
    sockaddr_storage storage{};
    Length length = sizeof(storage);
    if (!isOpen() || getsockname(native(handle_), reinterpret_cast<::sockaddr*>(&storage), &length) != 0) return Address();
    return Address::fromSockaddr(&storage, static_cast<size_t>(length));
}

void Socket::setVoiceTrafficClass() {
    if (!isOpen()) return;
    // DSCP 46 (expedited forwarding) in the upper six bits; WMM maps it to the voice queue.
    const int tos = 46 << 2;
    const auto* value = reinterpret_cast<const char*>(&tos);
    setsockopt(native(handle_), IPPROTO_IP, IP_TOS, value, sizeof(tos));
#if defined(IPV6_TCLASS)
    setsockopt(native(handle_), IPPROTO_IPV6, IPV6_TCLASS, value, sizeof(tos));
#endif
}

// MARK: - UDP

namespace {
/// IPv6 sockets also take IPv4 traffic (as mapped addresses), so one socket serves both.
void allowIPv4(intptr_t handle, int family) {
    if (family != AF_INET6) return;
    const int off = 0;
    setsockopt(native(handle), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
}
}  // namespace

bool UDPSocket::open(int family) {
    if (!create(family, SOCK_DGRAM, IPPROTO_UDP)) return false;
    family_ = family;
    allowIPv4(handle_, family);
#if defined(_WIN32)
    // Otherwise one ICMP "port unreachable" (a peer that went away) fails the next receive of
    // an unconnected socket, whoever it was from.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(native(handle_), SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
    return true;
}

bool UDPSocket::bind(uint16_t port) {
    sockaddr_storage storage{};
    Length length = 0;
    if (family_ == AF_INET6) {
        auto* address = reinterpret_cast<sockaddr_in6*>(&storage);
        address->sin6_family = AF_INET6;
        address->sin6_addr = in6addr_any;
        address->sin6_port = htons(port);
        length = sizeof(sockaddr_in6);
    } else {
        auto* address = reinterpret_cast<sockaddr_in*>(&storage);
        address->sin_family = AF_INET;
        address->sin_addr.s_addr = htonl(INADDR_ANY);
        address->sin_port = htons(port);
        length = sizeof(sockaddr_in);
    }
    return isOpen() && ::bind(native(handle_), reinterpret_cast<const ::sockaddr*>(&storage), length) == 0;
}

bool UDPSocket::connect(const Address& address) {
    return isOpen() && ::connect(native(handle_), static_cast<const ::sockaddr*>(address.sockaddr()), static_cast<Length>(address.length())) == 0;
}

bool UDPSocket::send(const uint8_t* data, size_t size) {
    if (!isOpen()) return false;
    return ::send(native(handle_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0) == static_cast<int>(size);
}

bool UDPSocket::sendTo(const uint8_t* data, size_t size, const Address& address) {
    if (!isOpen()) return false;
    return ::sendto(native(handle_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0, static_cast<const ::sockaddr*>(address.sockaddr()),
               static_cast<Length>(address.length()))
        == static_cast<int>(size);
}

int UDPSocket::receive(uint8_t* buffer, size_t capacity, int timeoutMilliseconds, Address* from) {
    const int ready = waitReadable(timeoutMilliseconds);
    if (ready <= 0) return ready;
    sockaddr_storage storage{};
    Length length = sizeof(storage);
    const auto received = ::recvfrom(native(handle_), reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0, reinterpret_cast<::sockaddr*>(&storage), &length);
    if (received < 0) {
#if defined(_WIN32)
        // The datagram was longer than the buffer; it was cut, which callers treat as garbage.
        if (lastError() == WSAEMSGSIZE) return static_cast<int>(capacity);
#endif
        return -1;
    }
    if (from != nullptr) *from = Address::fromSockaddr(&storage, static_cast<size_t>(length));
    return static_cast<int>(received);
}

// MARK: - TCP

bool TCPSocket::connect(const Address& address, int timeoutMilliseconds, const std::atomic<bool>* stop) {
    if (!create(address.family(), SOCK_STREAM, IPPROTO_TCP)) return false;
    const Native socket = native(handle_);
    setNonBlocking(socket, true);
    bool connected = ::connect(socket, static_cast<const ::sockaddr*>(address.sockaddr()), static_cast<Length>(address.length())) == 0;
    if (!connected && wouldBlock(lastError())) {
        // Wait in slices so a stop request is noticed.
        for (int waited = 0; waited < timeoutMilliseconds && !(stop != nullptr && stop->load()); waited += 100) {
            const int ready = waitFor(socket, POLLOUT, std::min(100, timeoutMilliseconds - waited));
            if (ready < 0) break;
            if (ready > 0) {
                int error = 0;
                Length length = sizeof(error);
                connected = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
                break;
            }
        }
    }
    if (!connected) {
        close();
        return false;
    }
    setNonBlocking(socket, false);
    const int one = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return true;
}

bool TCPSocket::sendAll(const uint8_t* data, size_t size) {
    while (size > 0 && isOpen()) {
#if defined(__linux__)
        const auto sent = ::send(native(handle_), data, size, MSG_NOSIGNAL);
#else
        const auto sent = ::send(native(handle_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
#endif
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return size == 0;
}

TCPSocket::ReadResult TCPSocket::readExactly(uint8_t* buffer, size_t size, const std::atomic<bool>& stop) {
    while (size > 0) {
        if (stop.load(std::memory_order_relaxed)) return ReadResult::stopped;
        const int ready = waitReadable(100);
        if (ready < 0) return ReadResult::failed;
        if (ready == 0) continue;
        const auto received = ::recv(native(handle_), reinterpret_cast<char*>(buffer), static_cast<int>(size), 0);
        if (received == 0) return ReadResult::closed;
        if (received < 0) return ReadResult::failed;
        buffer += received;
        size -= static_cast<size_t>(received);
    }
    return ReadResult::ok;
}

void TCPSocket::adopt(intptr_t handle) {
    close();
    handle_ = handle;
    const int one = 1;
    setsockopt(native(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(__APPLE__)
    setsockopt(native(handle_), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

// MARK: - TCP listener

bool TCPListener::listen(int family, uint16_t port) {
    if (!create(family, SOCK_STREAM, IPPROTO_TCP)) return false;
    allowIPv4(handle_, family);
    const Native socket = native(handle_);
    sockaddr_storage storage{};
    Length length = 0;
    if (family == AF_INET6) {
        auto* address = reinterpret_cast<sockaddr_in6*>(&storage);
        address->sin6_family = AF_INET6;
        address->sin6_addr = in6addr_any;
        address->sin6_port = htons(port);
        length = sizeof(sockaddr_in6);
    } else {
        auto* address = reinterpret_cast<sockaddr_in*>(&storage);
        address->sin_family = AF_INET;
        address->sin_addr.s_addr = htonl(INADDR_ANY);
        address->sin_port = htons(port);
        length = sizeof(sockaddr_in);
    }
    if (::bind(socket, reinterpret_cast<const ::sockaddr*>(&storage), length) != 0 || ::listen(socket, SOMAXCONN) != 0) {
        close();
        return false;
    }
    return true;
}

TCPSocket TCPListener::accept(int timeoutMilliseconds, Address* peer) {
    TCPSocket result;
    if (waitReadable(timeoutMilliseconds) <= 0) return result;
    sockaddr_storage storage{};
    Length length = sizeof(storage);
    const Native socket = ::accept(native(handle_), reinterpret_cast<::sockaddr*>(&storage), &length);
    if (socket == kInvalid) return result;
    // Accepted sockets may inherit the listener's non-blocking mode on some systems.
    setNonBlocking(socket, false);
    result.adopt(wrap(socket));
    if (peer != nullptr) *peer = Address::fromSockaddr(&storage, static_cast<size_t>(length));
    return result;
}

}  // namespace asyncmusic::net
