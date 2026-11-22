#include "kv/net/socket.hpp"

#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace kv::net {

namespace {
std::string describe(int err) {
#ifdef _WIN32
    return "socket error " + std::to_string(err);
#else
    return std::string(std::strerror(err));
#endif
}
}  // namespace

bool net_init(std::string& err) {
#ifdef _WIN32
    WSADATA data;
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        err = "WSAStartup failed: " + std::to_string(rc);
        return false;
    }
    (void)err;
    return true;
#else
    (void)err;
    return true;
#endif
}

void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

socket_t tcp_listen(const std::string& addr, std::uint16_t port, int backlog, bool reuse_port,
                    std::string& err) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        err = "socket() failed: " + describe(last_socket_error());
        return kInvalidSocket;
    }

    const int one = 1;
    // SO_REUSEADDR on both platforms; SO_REUSEPORT only where it exists, which
    // is what lets several reactor threads share the port on Linux.
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_REUSEPORT
    if (reuse_port) {
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof(one));
    }
#else
    (void)reuse_port;
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (addr.empty() || addr == "0.0.0.0") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, addr.c_str(), &address.sin_addr) != 1) {
        err = "invalid bind address: " + addr;
        close_socket(fd);
        return kInvalidSocket;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        err = "bind(" + addr + ":" + std::to_string(port) + ") failed: " +
              describe(last_socket_error());
        close_socket(fd);
        return kInvalidSocket;
    }
    if (::listen(fd, backlog) != 0) {
        err = "listen() failed: " + describe(last_socket_error());
        close_socket(fd);
        return kInvalidSocket;
    }
    return fd;
}

bool set_nonblocking(socket_t fd, std::string& err) {
#ifdef _WIN32
    u_long mode = 1;
    if (::ioctlsocket(fd, FIONBIO, &mode) != 0) {
        err = "ioctlsocket(FIONBIO) failed: " + describe(last_socket_error());
        return false;
    }
    return true;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        err = "fcntl(O_NONBLOCK) failed: " + describe(last_socket_error());
        return false;
    }
    return true;
#endif
}

bool set_nodelay(socket_t fd) {
    const int one = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                        sizeof(one)) == 0;
}

void close_socket(socket_t fd) noexcept {
    if (fd == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

int last_socket_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool would_block(int err) noexcept {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool interrupted(int err) noexcept {
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

std::string error_string(int err) { return describe(err); }

}  // namespace kv::net
