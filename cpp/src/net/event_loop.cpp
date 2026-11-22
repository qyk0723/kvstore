#include "kv/net/event_loop.hpp"

#include <cstring>
#include <utility>

namespace kv::net {

namespace {

constexpr std::size_t kMaxPendingOutput = 8u << 20;  // back-pressure ceiling
constexpr int kPollTimeoutMs = 200;

std::string format_peer(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        return "?";
    }
    return std::string(buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

std::string arity_error(const char* command) {
    return std::string("wrong number of arguments for '") + command + "'";
}

}  // namespace

// ===========================================================================
// CommandHandler
// ===========================================================================

CommandHandler::Reply CommandHandler::error(std::string message) const {
    Reply reply;
    reply.data = "ERROR:" + std::move(message);
    return reply;
}

CommandHandler::Reply CommandHandler::execute(const Request& req) {
    Reply reply;

    switch (req.cmd) {
        case Command::Set: {
            if (req.args.size() != 3) {
                return error(arity_error("SET"));
            }
            if (store_.put(req.args[1], req.args[2]) != Status::Ok) {
                return error("set failed");
            }
            reply.data = "SUCCESS";
            return reply;
        }
        case Command::Get: {
            if (req.args.size() != 2) {
                return error(arity_error("GET"));
            }
            const auto value = store_.get(req.args[1]);
            if (!value.has_value()) {
                reply.data = "NO EXIST";
                return reply;
            }
            reply.data = "SUCCESS:";
            reply.data += *value;
            return reply;
        }
        case Command::Del: {
            if (req.args.size() != 2) {
                return error(arity_error("DEL"));
            }
            reply.data = store_.erase(req.args[1]) == Status::Ok ? "SUCCESS" : "NO EXIST";
            return reply;
        }
        case Command::Mod: {
            if (req.args.size() != 3) {
                return error(arity_error("MOD"));
            }
            reply.data = store_.update(req.args[1], req.args[2]) == Status::Ok ? "SUCCESS"
                                                                              : "NO EXIST";
            return reply;
        }
        case Command::Count:
            reply.data = std::to_string(store_.size());
            return reply;
        case Command::Keys: {
            if (req.args.size() > 2) {
                return error(arity_error("KEYS"));
            }
            const std::string_view prefix =
                req.args.size() == 2 ? std::string_view(req.args[1]) : std::string_view{};
            // KEYS is O(n) and materialises the whole answer in one reply, so a
            // full scan of a big keyspace would both stall the shard and blow
            // through the per-connection write buffer.  Refusing loudly is
            // better than silently truncating the key list.
            constexpr std::size_t kMaxKeys = 1000;
            std::size_t matched = 0;
            std::string joined;
            bool overflow = false;

            store_.for_each([&](std::string_view key, std::string_view) {
                if (!prefix.empty()) {
                    if (key.size() < prefix.size() ||
                        key.compare(0, prefix.size(), prefix) != 0) {
                        return;
                    }
                }
                ++matched;
                if (matched > kMaxKeys) {
                    overflow = true;
                    return;
                }
                if (!joined.empty()) {
                    joined.push_back(' ');
                }
                joined.append(key);
            });

            if (overflow) {
                return error("too many keys (" + std::to_string(matched) +
                             "), narrow the prefix");
            }
            reply.data = "SUCCESS:";
            reply.data += joined;
            return reply;
        }
        case Command::Ping:
            reply.data = "PONG";
            return reply;
        case Command::Flush:
            store_.clear();
            reply.data = "SUCCESS";
            return reply;
        case Command::Info:
            reply.data = "keys:" + std::to_string(store_.size()) +
                         ",engine:" + store_.engine_name() +
                         ",shards:" + std::to_string(store_.shard_count()) +
                         ",commands:" +
                         std::to_string(stats_.commands.load(std::memory_order_relaxed)) +
                         ",connections:" +
                         std::to_string(stats_.connections_opened.load(std::memory_order_relaxed)) +
                         ",poller:" + Poller::backend_name();
            return reply;
        case Command::Quit:
            reply.data = "BYE";
            reply.close = true;
            return reply;
        case Command::Unknown:
            break;
    }
    // The original server asserted here and took the whole process down.
    return error("unknown command '" + req.args[0] + "'");
}

// ===========================================================================
// EventLoop
// ===========================================================================

EventLoop::EventLoop(ShardedStore& store, Stats& stats, EventLoopOptions options)
    : stats_(stats), options_(std::move(options)), handler_(store, stats) {
    ready_.resize(256);
}

EventLoop::~EventLoop() {
    for (auto& entry : connections_) {
        poller_ && poller_->del(entry.first);
        close_socket(entry.first);
    }
    connections_.clear();
    if (listen_fd_ != kInvalidSocket) {
        close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }
    poller_.reset();
}

bool EventLoop::init(std::string& err) {
    poller_ = Poller::create();
    if (!poller_) {
        err = "failed to create a poller";
        return false;
    }

    listen_fd_ =
        tcp_listen(options_.bind_addr, options_.port, 512, options_.reuse_port, err);
    if (listen_fd_ == kInvalidSocket) {
        return false;
    }
    if (!set_nonblocking(listen_fd_, err)) {
        return false;
    }
    if (!poller_->add(listen_fd_, kEventRead, err)) {
        return false;
    }

    sockaddr_in local{};
    socklen_type local_length = static_cast<socklen_type>(sizeof(local));
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&local), &local_length) == 0) {
        bound_port_ = ntohs(local.sin_port);
    } else {
        bound_port_ = options_.port;
    }

    listen_address_ = options_.bind_addr + ":" + std::to_string(bound_port_);
    return true;
}

void EventLoop::run() {
    while (running_.load(std::memory_order_relaxed)) {
        const int count = poller_->wait(ready_.data(), static_cast<int>(ready_.size()),
                                        kPollTimeoutMs);
        for (int i = 0; i < count; ++i) {
            handle_event(ready_[static_cast<std::size_t>(i)]);
            if (!running_.load(std::memory_order_relaxed)) {
                break;
            }
        }
    }
}

void EventLoop::handle_event(const ReadyEvent& event) {
    if (event.fd == listen_fd_) {
        accept_ready();
        return;
    }

    if (connections_.find(event.fd) == connections_.end()) {
        return;
    }
    if (event.failed) {
        destroy(event.fd);
        return;
    }

    // Read and write are independent interests.  The C server chained them
    // with `else if`, so a connection that was ready for both only ever got
    // read from and its reply sat in the buffer.
    if ((event.events & kEventRead) != 0) {
        read_ready(event.fd);
        if (connections_.find(event.fd) == connections_.end()) {
            return;
        }
    }
    if ((event.events & kEventWrite) != 0) {
        write_ready(event.fd);
    }
}

void EventLoop::accept_ready() {
    for (;;) {
        sockaddr_in address{};
        socklen_type length = static_cast<socklen_type>(sizeof(address));
        const socket_t fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        if (fd == kInvalidSocket) {
            const int err = last_socket_error();
            if (interrupted(err)) {
                continue;
            }
            break;  // EAGAIN: the backlog is drained
        }

        if (connections_.size() >= options_.max_connections) {
            close_socket(fd);  // overload protection
            continue;
        }

        std::string err;
        if (!set_nonblocking(fd, err)) {
            close_socket(fd);
            continue;
        }
        set_nodelay(fd);

        if (!poller_->add(fd, kEventRead, err)) {
            close_socket(fd);
            continue;
        }

        auto connection = std::make_unique<Connection>();
        connection->fd = fd;
        connection->events = kEventRead;
        connection->peer = format_peer(address);
        connections_.emplace(fd, std::move(connection));
        stats_.connections_opened.fetch_add(1, std::memory_order_relaxed);
    }
}

void EventLoop::read_ready(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = *it->second;
    const std::size_t chunk = options_.read_chunk;

    for (;;) {
        char* destination = conn.parser.prepare_read(chunk);
        const io_result got = ::recv(fd, destination, static_cast<int>(chunk), 0);
        if (got > 0) {
            conn.parser.commit_read(static_cast<std::size_t>(got));
            stats_.bytes_read.fetch_add(static_cast<std::uint64_t>(got),
                                        std::memory_order_relaxed);
            if (static_cast<std::size_t>(got) < chunk) {
                break;  // drained
            }
            continue;
        }
        if (got == 0) {
            destroy(fd);  // orderly shutdown by the peer
            return;
        }
        const int err = last_socket_error();
        if (interrupted(err)) {
            continue;
        }
        if (would_block(err)) {
            break;
        }
        destroy(fd);
        return;
    }

    process_input(fd);
}

void EventLoop::process_input(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = *it->second;

    Request request;
    for (;;) {
        const RequestParser::Result result = conn.parser.next(request);
        if (result == RequestParser::Result::Incomplete) {
            break;
        }
        if (result == RequestParser::Result::Error) {
            conn.output.append("ERROR:" + conn.parser.last_error());
            conn.output.append("\r\n");
            stats_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
            conn.close_after_flush = true;
            break;
        }

        const CommandHandler::Reply reply = handler_.execute(request);
        conn.output.append(reply.data);
        conn.output.append("\r\n");
        stats_.commands.fetch_add(1, std::memory_order_relaxed);

        if (reply.close) {
            conn.close_after_flush = true;
            break;
        }
        if (conn.output.size() > kMaxPendingOutput) {
            // A client that issues requests faster than it reads replies must
            // not be able to grow the server's heap without bound.
            destroy(fd);
            return;
        }
    }

    flush_output(fd);
}

void EventLoop::flush_output(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = *it->second;

    while (!conn.output.empty()) {
        const std::string_view pending = conn.output.readable();
        const io_result sent =
            ::send(fd, pending.data(), static_cast<int>(pending.size()), kSendFlags);
        if (sent > 0) {
            conn.output.consume(static_cast<std::size_t>(sent));
            stats_.bytes_written.fetch_add(static_cast<std::uint64_t>(sent),
                                           std::memory_order_relaxed);
            continue;
        }
        if (sent == 0) {
            break;
        }
        const int err = last_socket_error();
        if (interrupted(err)) {
            continue;
        }
        if (would_block(err)) {
            break;  // socket buffer full: finish later from the EPOLLOUT branch
        }
        destroy(fd);
        return;
    }

    if (conn.output.empty() && conn.close_after_flush) {
        destroy(fd);
        return;
    }
    update_interest(fd);
}

void EventLoop::write_ready(socket_t fd) { flush_output(fd); }

void EventLoop::update_interest(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = *it->second;

    std::uint32_t desired = kEventRead;
    if (!conn.output.empty()) {
        desired |= kEventWrite;
    }
    if (desired == conn.events) {
        return;
    }
    std::string err;
    if (poller_->mod(fd, desired, err)) {
        conn.events = desired;
    }
}

void EventLoop::destroy(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    if (poller_) {
        poller_->del(fd);
    }
    close_socket(fd);
    connections_.erase(it);
    stats_.connections_closed.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace kv::net
