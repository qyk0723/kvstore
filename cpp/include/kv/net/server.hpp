#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kv/net/event_loop.hpp"
#include "kv/net/socket.hpp"
#include "kv/store.hpp"
#include "kv/wal.hpp"

namespace kv::net {

struct ServerConfig {
    std::string bind_addr = "0.0.0.0";
    std::uint16_t port = 9096;
    int threads = 1;
    std::size_t max_connections = 8192;
    std::size_t shards = 16;
    std::string engine = "hash";
    std::string db_path;   // empty => in-memory only
    bool fsync_each_write = false;
    std::size_t max_request_bytes = 64 * 1024;
};

// The server owns the store (and optionally the WAL); each reactor thread
// owns exactly one EventLoop and, on Linux, one SO_REUSEPORT listener, so the
// kernel spreads accepted connections across the threads without a shared
// acceptor bottleneck.
class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocks until request_stop() is called or a fatal initialisation error
    // occurs.  Returns 0 on success.
    int run();

    void request_stop() noexcept { stop_.store(true); }

    const ServerConfig& config() const noexcept { return config_; }

private:
    bool prepare(std::string& err);
    void shutdown();

    ServerConfig config_;
    std::unique_ptr<Wal> wal_;
    std::unique_ptr<ShardedStore> store_;
    std::unique_ptr<Stats> stats_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
};

// Installs SIGINT/SIGTERM handlers that flip `flag`, and ignores SIGPIPE so a
// peer disappearing mid-send cannot kill the process.  Kept out of Server so
// the signal policy is explicit at the call site.
void install_stop_handler(std::atomic<bool>* flag);

}  // namespace kv::net
