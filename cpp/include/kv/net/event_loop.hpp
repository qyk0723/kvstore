#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "kv/buffer.hpp"
#include "kv/net/poller.hpp"
#include "kv/net/socket.hpp"
#include "kv/protocol.hpp"
#include "kv/store.hpp"

namespace kv::net {

struct Stats {
    std::atomic<std::uint64_t> commands{0};
    std::atomic<std::uint64_t> connections_opened{0};
    std::atomic<std::uint64_t> connections_closed{0};
    std::atomic<std::uint64_t> bytes_read{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> protocol_errors{0};
};

// Executes one parsed request against the store and renders the reply.
class CommandHandler {
public:
    CommandHandler(ShardedStore& store, Stats& stats) : store_(store), stats_(stats) {}

    struct Reply {
        std::string data;
        bool close = false;
    };

    Reply execute(const Request& req);

private:
    Reply error(std::string message) const;

    ShardedStore& store_;
    Stats& stats_;
};

struct EventLoopOptions {
    std::string bind_addr = "0.0.0.0";
    std::uint16_t port = 9096;
    std::size_t max_connections = 8192;
    bool reuse_port = false;
    std::size_t read_chunk = 8192;
};

// One Reactor: one listening socket, one poller, one thread.
//
// Event handling compared to the original:
//   * readable and writable are handled independently (the C code used
//     `else if`, so a connection ready for both never got its reply flushed);
//   * sockets are non-blocking and drained until EAGAIN;
//   * partial send() is handled through the pending-write buffer instead of
//     assuming the whole reply went out;
//   * peer-closed/hard-error events close the connection instead of being
//     silently ignored;
//   * connections live in a hash map keyed by fd instead of a fixed
//     `connlist[1024]` indexed by fd, so there is no out-of-bounds write once
//     fds exceed 1023.
class EventLoop {
public:
    EventLoop(ShardedStore& store, Stats& stats, EventLoopOptions options);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool init(std::string& err);
    void run();
    void stop() noexcept { running_.store(false); }

    const std::string& listen_address() const noexcept { return listen_address_; }
    // Actual port in use; useful when the caller asked for port 0 (ephemeral).
    std::uint16_t bound_port() const noexcept { return bound_port_; }
    std::size_t connection_count() const noexcept { return connections_.size(); }

private:
    struct Connection {
        socket_t fd = kInvalidSocket;
        Buffer output;
        std::uint32_t events = 0;  // currently registered with the poller
        bool close_after_flush = false;
        RequestParser parser;  // owns the reassembly buffer
        std::string peer;
    };

    void handle_event(const ReadyEvent& event);
    void accept_ready();
    void read_ready(socket_t fd);
    void write_ready(socket_t fd);
    void process_input(socket_t fd);
    void flush_output(socket_t fd);
    void update_interest(socket_t fd);
    void destroy(socket_t fd);

    Stats& stats_;
    EventLoopOptions options_;
    CommandHandler handler_;
    std::unique_ptr<Poller> poller_;
    socket_t listen_fd_ = kInvalidSocket;
    std::unordered_map<socket_t, std::unique_ptr<Connection>> connections_;
    std::vector<ReadyEvent> ready_;
    std::atomic<bool> running_{true};
    std::string listen_address_;
    std::uint16_t bound_port_ = 0;
};

}  // namespace kv::net
