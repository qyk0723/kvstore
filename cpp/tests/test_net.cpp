#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kv/net/event_loop.hpp"

namespace {

using kv::net::close_socket;
using kv::net::EventLoop;
using kv::net::EventLoopOptions;
using kv::net::io_result;
using kv::net::kInvalidSocket;
using kv::net::kSendFlags;
using kv::net::socket_t;
using kv::net::Stats;
using kv::ShardedStore;

socket_t connect_to(std::uint16_t port) {
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        return kInvalidSocket;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  static_cast<kv::net::socklen_type>(sizeof(address))) != 0) {
        close_socket(fd);
        return kInvalidSocket;
    }
    // Never let a broken server hang the test suite.
#ifdef _WIN32
    DWORD timeout = 5000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    return fd;
}

bool send_all(socket_t fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = static_cast<int>(data.size() - sent);
        const io_result written = ::send(fd, data.data() + sent, remaining, kSendFlags);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

std::string read_lines(socket_t fd, int expected_lines) {
    std::string out;
    char buffer[8192];
    for (;;) {
        int lines = 0;
        for (std::size_t i = 0; i + 1 < out.size(); ++i) {
            if (out[i] == '\r' && out[i + 1] == '\n') {
                ++lines;
            }
        }
        if (lines >= expected_lines) {
            break;
        }
        const io_result got = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (got <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(got));
    }
    return out;
}

// Splits a "PREFIX:a b c\r\n" reply into sorted words, so tests do not depend
// on the engine's iteration order.
std::vector<std::string> split_reply(std::string_view reply, std::string_view prefix) {
    std::vector<std::string> words;
    if (reply.size() < prefix.size() || reply.compare(0, prefix.size(), prefix) != 0) {
        return words;
    }
    std::string_view rest = reply.substr(prefix.size());
    if (rest.size() >= 2 && rest.substr(rest.size() - 2) == "\r\n") {
        rest.remove_suffix(2);
    }
    std::size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && rest[pos] == ' ') {
            ++pos;
        }
        if (pos >= rest.size()) {
            break;
        }
        const std::size_t start = pos;
        while (pos < rest.size() && rest[pos] != ' ') {
            ++pos;
        }
        words.emplace_back(rest.substr(start, pos - start));
    }
    std::sort(words.begin(), words.end());
    return words;
}

// Spins up one reactor on an ephemeral port and tears it down afterwards.
class Fixture {
public:
    explicit Fixture(const char* engine = "hash") : store(engine, 4) {}
    ~Fixture() {
        if (loop) {
            loop->stop();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool start() {
        EventLoopOptions options;
        options.bind_addr = "127.0.0.1";
        options.port = 0;  // let the kernel choose
        loop = std::make_unique<EventLoop>(store, stats, options);
        std::string error;
        if (!loop->init(error)) {
            return false;
        }
        port = loop->bound_port();
        worker = std::thread([this] { loop->run(); });
        return true;
    }

    ShardedStore store;
    Stats stats;
    std::unique_ptr<EventLoop> loop;
    std::thread worker;
    std::uint16_t port = 0;
};

}  // namespace

KV_TEST(net_basic_commands) {
    Fixture fixture;
    const bool started = fixture.start();
    KV_CHECK(started);
    if (!started) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    KV_CHECK(send_all(fd, "SET hello world\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS\r\n");

    KV_CHECK(send_all(fd, "GET hello\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS:world\r\n");

    KV_CHECK(send_all(fd, "MOD hello there\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS\r\n");
    KV_CHECK(send_all(fd, "GET hello\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS:there\r\n");

    KV_CHECK(send_all(fd, "GET missing\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "NO EXIST\r\n");

    KV_CHECK(send_all(fd, "COUNT\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "1\r\n");

    KV_CHECK(send_all(fd, "PING\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "PONG\r\n");

    KV_CHECK(send_all(fd, "DEL hello\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS\r\n");
    KV_CHECK(send_all(fd, "DEL hello\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "NO EXIST\r\n");

    KV_CHECK(send_all(fd, "COUNT\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "0\r\n");

    close_socket(fd);
}

// Regression: the C server called assert(0) on an unknown command and the
// whole process died.  Here it must be a normal error reply and the connection
// must stay usable.
KV_TEST(net_unknown_command_keeps_server_alive) {
    Fixture fixture;
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    KV_CHECK(send_all(fd, "FROBNICATE args\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "ERROR:unknown command 'FROBNICATE'\r\n");

    KV_CHECK(send_all(fd, "PING\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "PONG\r\n");

    // Wrong arity is an error, not a crash either.
    KV_CHECK(send_all(fd, "SET onlykey\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "ERROR:wrong number of arguments for 'SET'\r\n");

    KV_CHECK(send_all(fd, "PING\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "PONG\r\n");

    close_socket(fd);
}

KV_TEST(net_handles_pipelining) {
    Fixture fixture;
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    constexpr int kCount = 200;
    std::string wire;
    for (int i = 0; i < kCount; ++i) {
        wire += "SET k" + std::to_string(i) + " v" + std::to_string(i) + "\r\n";
    }
    KV_CHECK(send_all(fd, wire));

    const std::string replies = read_lines(fd, kCount);
    std::size_t lines = 0;
    for (std::size_t i = 0; i + 1 < replies.size(); ++i) {
        if (replies[i] == '\r' && replies[i + 1] == '\n') {
            ++lines;
        }
    }
    KV_CHECK_EQ(lines, kCount);
    KV_CHECK(replies.find("ERROR") == std::string::npos);

    KV_CHECK(send_all(fd, "COUNT\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), std::to_string(kCount) + "\r\n");

    close_socket(fd);
}

// The core "TCP is a stream" case: a request delivered in two writes.
KV_TEST(net_reassembles_split_request) {
    Fixture fixture;
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    KV_CHECK(send_all(fd, "SET spl"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    KV_CHECK(send_all(fd, "it ok\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS\r\n");

    KV_CHECK(send_all(fd, "GET spl"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    KV_CHECK(send_all(fd, "it\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS:ok\r\n");

    close_socket(fd);
}

KV_TEST(net_large_value_roundtrip) {
    Fixture fixture;
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    const std::string payload(20000, 'q');
    KV_CHECK(send_all(fd, "SET big " + payload + "\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS\r\n");

    KV_CHECK(send_all(fd, "GET big\r\n"));
    const std::string reply = read_lines(fd, 1);
    KV_CHECK(reply == "SUCCESS:" + payload + "\r\n");

    close_socket(fd);
}

KV_TEST(net_quit_closes_the_connection) {
    Fixture fixture;
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    KV_CHECK(send_all(fd, "QUIT\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "BYE\r\n");

    // The server closes after flushing, so the next read must report EOF.
    char buffer[16];
    const io_result got = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
    KV_CHECK(got <= 0);

    close_socket(fd);
}

KV_TEST(net_info_reports_engine) {
    Fixture fixture("rbtree");
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    KV_CHECK(send_all(fd, "INFO\r\n"));
    const std::string reply = read_lines(fd, 1);
    KV_CHECK(reply.find("engine:rbtree") != std::string::npos);
    KV_CHECK(reply.find("shards:4") != std::string::npos);

    close_socket(fd);
}

KV_TEST(net_survives_abrupt_client_disconnect) {
    Fixture fixture;
    KV_CHECK(fixture.start());
    if (!fixture.port) {
        return;
    }

    // Connect, send half a request, then vanish without closing politely.
    for (int i = 0; i < 20; ++i) {
        const socket_t fd = connect_to(fixture.port);
        KV_CHECK(fd != kInvalidSocket);
        if (fd == kInvalidSocket) {
            return;
        }
        KV_CHECK(send_all(fd, "SET partial"));
        close_socket(fd);
    }

    // The server must still serve new clients afterwards.
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }
    KV_CHECK(send_all(fd, "PING\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "PONG\r\n");
    close_socket(fd);
}

KV_TEST(net_keys_lists_and_filters) {
    Fixture fixture;
    const bool started = fixture.start();
    KV_CHECK(started);
    if (!started) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    KV_CHECK(send_all(fd, "SET user:1 alice\r\nSET user:2 bob\r\nSET post:1 hi\r\n"));
    KV_CHECK_STR(read_lines(fd, 3), "SUCCESS\r\nSUCCESS\r\nSUCCESS\r\n");

    // No prefix: every key, order-insensitive.
    KV_CHECK(send_all(fd, "KEYS\r\n"));
    const auto all = split_reply(read_lines(fd, 1), "SUCCESS:");
    KV_CHECK_EQ(all.size(), 3);
    if (all.size() == 3) {
        KV_CHECK_STR(all[0], "post:1");
        KV_CHECK_STR(all[1], "user:1");
        KV_CHECK_STR(all[2], "user:2");
    }

    // Prefix filter.
    KV_CHECK(send_all(fd, "KEYS user:\r\n"));
    const auto users = split_reply(read_lines(fd, 1), "SUCCESS:");
    KV_CHECK_EQ(users.size(), 2);
    if (users.size() == 2) {
        KV_CHECK_STR(users[0], "user:1");
        KV_CHECK_STR(users[1], "user:2");
    }

    // No match is an empty success, not an error.
    KV_CHECK(send_all(fd, "KEYS nomatch:\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "SUCCESS:\r\n");

    // Erased keys disappear from the listing.
    KV_CHECK(send_all(fd, "DEL user:2\r\nKEYS user:\r\n"));
    KV_CHECK_STR(read_lines(fd, 2), "SUCCESS\r\nSUCCESS:user:1\r\n");

    // Too many arguments is an arity error, not a crash.
    KV_CHECK(send_all(fd, "KEYS a b\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "ERROR:wrong number of arguments for 'KEYS'\r\n");

    // And the connection is still healthy.
    KV_CHECK(send_all(fd, "PING\r\n"));
    KV_CHECK_STR(read_lines(fd, 1), "PONG\r\n");

    close_socket(fd);
}

KV_TEST(net_keys_refuses_oversized_result) {
    Fixture fixture;
    const bool started = fixture.start();
    KV_CHECK(started);
    if (!started) {
        return;
    }
    const socket_t fd = connect_to(fixture.port);
    KV_CHECK(fd != kInvalidSocket);
    if (fd == kInvalidSocket) {
        return;
    }

    // 1001 keys: one past the reply cap.
    constexpr int kCount = 1001;
    std::string wire;
    for (int i = 0; i < kCount; ++i) {
        wire += "SET k" + std::to_string(i) + " v\r\n";
    }
    KV_CHECK(send_all(fd, wire));
    const std::string acks = read_lines(fd, kCount);
    std::size_t lines = 0;
    for (std::size_t i = 0; i + 1 < acks.size(); ++i) {
        if (acks[i] == '\r' && acks[i + 1] == '\n') {
            ++lines;
        }
    }
    KV_CHECK_EQ(lines, kCount);

    KV_CHECK(send_all(fd, "KEYS\r\n"));
    const std::string reply = read_lines(fd, 1);
    KV_CHECK(reply.rfind("ERROR:too many keys", 0) == 0);

    // A narrow prefix still works.
    KV_CHECK(send_all(fd, "KEYS k99\r\n"));
    const auto subset = split_reply(read_lines(fd, 1), "SUCCESS:");
    KV_CHECK(subset.size() > 0);
    KV_CHECK(subset.size() <= 11);  // k99, k990..k999

    close_socket(fd);
}
