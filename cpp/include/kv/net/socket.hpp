#pragma once

#include <cstdint>
#include <string>

// Thin portability layer over the BSD sockets API.
//
// The production target is Linux + epoll.  The socket wrapper is kept
// separate from the reactor so the same Connection state machine can be built
// and tested on Windows with a poll() backend, which is what makes it possible
// to run the whole test suite off-Linux.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  // WSAPoll
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kv::net {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
using socklen_type = int;
using io_result = int;
constexpr int kSendFlags = 0;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
using socklen_type = socklen_t;
using io_result = ssize_t;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

// Process-wide socket subsystem init/teardown (WSAStartup on Windows).
bool net_init(std::string& err);
void net_cleanup();

// Creates a listening socket bound to `addr:port`.  `reuse_port` sets
// SO_REUSEPORT where the platform has it (Linux), which lets several reactor
// threads each own a listener and have the kernel spread connections.
socket_t tcp_listen(const std::string& addr, std::uint16_t port, int backlog,
                    bool reuse_port, std::string& err);

bool set_nonblocking(socket_t fd, std::string& err);
bool set_nodelay(socket_t fd);

void close_socket(socket_t fd) noexcept;

int last_socket_error() noexcept;

// True for the "try again later" errors that a non-blocking socket reports.
bool would_block(int err) noexcept;
bool interrupted(int err) noexcept;

std::string error_string(int err);

}  // namespace kv::net
