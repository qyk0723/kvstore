#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kv/net/socket.hpp"

namespace kv::net {

enum Event : std::uint32_t {
    kEventNone = 0,
    kEventRead = 1u << 0,
    kEventWrite = 1u << 1,
};

struct ReadyEvent {
    socket_t fd = kInvalidSocket;
    std::uint32_t events = kEventNone;
    bool peer_closed = false;  // EOF or EPOLLRDHUP on the read side
    bool failed = false;       // hard error / POLLERR
};

// Minimal readiness-notification interface.
//
// Two backends:
//   * epoll  (Linux, the deployment target) -- chosen at compile time;
//   * poll   (Windows/other) -- used so the reactor and the whole test suite
//             also build and run on a developer machine without WSL.
//
// The reactor itself only sees this interface, so the backend is the only
// platform-specific code in the networking layer.
class Poller {
public:
    virtual ~Poller() = default;

    virtual bool add(socket_t fd, std::uint32_t events, std::string& err) = 0;
    virtual bool mod(socket_t fd, std::uint32_t events, std::string& err) = 0;
    virtual bool del(socket_t fd) = 0;

    // Returns the number of ready handles, 0 on timeout, -1 on error.
    virtual int wait(ReadyEvent* out, int max_events, int timeout_ms) = 0;

    static std::unique_ptr<Poller> create();
    static const char* backend_name() noexcept;
};

}  // namespace kv::net
