//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

/// Thin, portable wrappers over BSD sockets and Winsock, just what the group protocols need.
/// No platform headers leak out of here; everything is blocking with timeouts, so callers run
/// sockets on their own threads and stop them by flag.
namespace asyncmusic::net {

/// Initializes the socket library once per process (Winsock); a no-op elsewhere.
bool startup();

/// An IPv4 or IPv6 socket address.
class Address {
public:
    Address() = default;

    /// Parses a numeric IPv4 or IPv6 address (IPv6 may carry a `%scope`).
    static std::optional<Address> fromString(const std::string& host, uint16_t port);
    /// Copies a `sockaddr_in` or `sockaddr_in6`.
    static Address fromSockaddr(const void* address, size_t length);

    bool isValid() const { return length_ > 0; }
    int family() const;
    uint16_t port() const;
    Address withPort(uint16_t port) const;
    /// The numeric host, e.g. `192.168.1.2` or `fe80::1%4`.
    std::string host() const;
    /// `192.168.1.2:5000` or `[fe80::1%4]:5000`.
    std::string toString() const;

    const void* sockaddr() const { return storage_; }
    uint32_t length() const { return length_; }

    bool operator==(const Address& other) const;
    bool operator!=(const Address& other) const { return !(*this == other); }

private:
    // Large enough for sockaddr_storage on every platform.
    alignas(8) unsigned char storage_[128] = {};
    uint32_t length_ = 0;
};

/// The text of the calling thread's last socket error.
std::string lastErrorDescription();

/// Owns one socket handle. Move-only; closes on destruction.
class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool isOpen() const;
    void close();

    /// Waits until data (or an error, or the peer closing) can be read: 1 readable,
    /// 0 timed out, -1 failed.
    int waitReadable(int timeoutMilliseconds) const;

    Address localAddress() const;

    /// Marks outgoing packets for the voice access category (DSCP EF). Wi-Fi queues these ahead
    /// of bulk traffic, which tightens round-trip jitter. Best effort: some systems ignore it.
    void setVoiceTrafficClass();

protected:
    bool create(int family, int type, int protocol);

    intptr_t handle_ = -1;
};

class UDPSocket : public Socket {
public:
    bool open(int family);
    /// Binds to `port` (0: any) on all interfaces of the socket's family.
    bool bind(uint16_t port);
    bool connect(const Address& address);
    bool send(const uint8_t* data, size_t size);
    bool sendTo(const uint8_t* data, size_t size, const Address& address);
    /// Waits up to `timeoutMilliseconds` for a datagram. Returns its size (datagrams longer
    /// than `capacity` are cut), 0 on timeout, -1 on error (e.g. the peer's port was closed).
    int receive(uint8_t* buffer, size_t capacity, int timeoutMilliseconds, Address* from = nullptr);

private:
    int family_ = 0;
};

class TCPSocket : public Socket {
public:
    TCPSocket() = default;
    enum class ReadResult : uint8_t { ok, closed, failed, stopped };

    /// Connects within `timeoutMilliseconds`, giving up early once `stop` is set. Turns off
    /// Nagle's algorithm.
    bool connect(const Address& address, int timeoutMilliseconds, const std::atomic<bool>* stop = nullptr);
    /// Sends everything, blocking as long as needed.
    bool sendAll(const uint8_t* data, size_t size);
    /// Reads exactly `size` bytes, checking `stop` at least every 100 ms.
    ReadResult readExactly(uint8_t* buffer, size_t size, const std::atomic<bool>& stop);

private:
    friend class TCPListener;
    void adopt(intptr_t handle);
};

/// Accepts TCP connections.
class TCPListener : public Socket {
public:
    /// Listens on `port` (0: any free one) on all interfaces of `family`.
    bool listen(int family, uint16_t port);
    /// Waits up to `timeoutMilliseconds` for a connection; the result is closed if none came.
    TCPSocket accept(int timeoutMilliseconds, Address* peer = nullptr);
};

}  // namespace asyncmusic::net
