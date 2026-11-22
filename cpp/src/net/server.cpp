#include "kv/net/server.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace kv::net {

namespace {

std::atomic<bool>* g_stop_flag = nullptr;

extern "C" void on_signal(int) {
    if (g_stop_flag != nullptr) {
        g_stop_flag->store(true);
    }
}

}  // namespace

void install_stop_handler(std::atomic<bool>* flag) {
    g_stop_flag = flag;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    // Without this a write to a half-closed socket terminates the process.
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

Server::Server(ServerConfig config) : config_(std::move(config)) {}

Server::~Server() = default;

bool Server::prepare(std::string& err) {
    if (!make_engine(config_.engine)) {
        err = "unknown engine '" + config_.engine + "' (available: " + engine_names() + ")";
        return false;
    }
    if (config_.threads < 1) {
        config_.threads = 1;
    }
    if (config_.shards == 0) {
        config_.shards = 1;
    }
    if (config_.max_connections == 0) {
        config_.max_connections = 1;
    }

    stats_ = std::make_unique<Stats>();
    // Attach the WAL only after replay, so replayed writes are not re-logged.
    store_ = std::make_unique<ShardedStore>(config_.engine, config_.shards, nullptr);

    if (!config_.db_path.empty()) {
        wal_ = Wal::open(config_.db_path, config_.fsync_each_write, err);
        if (!wal_) {
            return false;
        }
        std::string note;
        const std::size_t replayed = wal_->replay(
            [this](Wal::Op op, std::string_view key, std::string_view value) {
                if (op == Wal::Op::Put) {
                    store_->put(key, value);
                } else if (op == Wal::Op::Erase) {
                    store_->erase(key);
                }
            },
            note);
        if (note.empty()) {
            std::fprintf(stderr, "[wal] replayed %zu record(s) from %s\n", replayed,
                         config_.db_path.c_str());
        } else {
            std::fprintf(stderr, "[wal] replayed %zu record(s) from %s (%s)\n", replayed,
                         config_.db_path.c_str(), note.c_str());
        }
        store_->set_wal(wal_.get());
    }

    return true;
}

int Server::run() {
    std::string err;
    if (!prepare(err)) {
        std::fprintf(stderr, "kvstore: %s\n", err.c_str());
        return 1;
    }
    install_stop_handler(&stop_);

    int thread_count = config_.threads;
#if !defined(__linux__)
    // SO_REUSEPORT is what lets several listeners share the port; without it
    // the extra binds would fail, so this build always runs one reactor.
    thread_count = 1;
#endif

    std::vector<std::unique_ptr<EventLoop>> loops;
    loops.reserve(static_cast<std::size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
        EventLoopOptions options;
        options.bind_addr = config_.bind_addr;
        options.port = config_.port;
        options.max_connections = config_.max_connections;
        options.reuse_port = thread_count > 1;
        auto loop = std::make_unique<EventLoop>(*store_, *stats_, std::move(options));
        if (!loop->init(err)) {
            std::fprintf(stderr, "kvstore: %s\n", err.c_str());
            return 1;
        }
        loops.push_back(std::move(loop));
    }

    std::printf("kvstore listening on %s  engine=%s shards=%zu reactors=%d poller=%s\n",
                loops.front()->listen_address().c_str(), config_.engine.c_str(), config_.shards,
                thread_count, Poller::backend_name());
    if (wal_) {
        std::printf("persistence: %s (fsync each write: %s)\n", config_.db_path.c_str(),
                    config_.fsync_each_write ? "yes" : "no");
    }
    std::fflush(stdout);

    threads_.reserve(loops.size());
    for (auto& loop : loops) {
        EventLoop* raw = loop.get();
        threads_.emplace_back([raw] { raw->run(); });
    }

    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto& loop : loops) {
        loop->stop();
    }
    for (auto& worker : threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    threads_.clear();

    if (wal_) {
        wal_->flush();
        wal_->sync();
    }

    const std::uint64_t commands = stats_->commands.load();
    std::printf("\nshutdown: %llu command(s), %zu key(s), poller=%s\n",
                static_cast<unsigned long long>(commands), store_->size(),
                Poller::backend_name());
    return 0;
}

void Server::shutdown() {
    stop_.store(true);
    if (wal_) {
        wal_->flush();
        wal_->sync();
    }
}

}  // namespace kv::net
