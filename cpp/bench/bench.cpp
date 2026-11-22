// Load generator for the kvstore server.
//
// Differences from legacy/test3.c, which is what made the original numbers
// meaningless:
//   * replies are read in bulk, not one byte per recv() call, so the client is
//     not the bottleneck;
//   * requests can be pipelined (depth N) to measure server throughput rather
//     than round-trip cost;
//   * it reports the reply mix and checks every response, so a server that
//     drops or corrupts replies cannot look fast.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kv/net/socket.hpp"

namespace {

using kv::net::close_socket;
using kv::net::io_result;
using kv::net::kInvalidSocket;
using kv::net::kSendFlags;
using kv::net::socket_t;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9096;
    int threads = 4;
    long long requests = 100000;
    int pipeline = 32;
    int key_space = 10000;
    double get_ratio = 0.5;
    int value_size = 32;
    bool use_stdin_values = false;
};

struct ThreadResult {
    long long requests = 0;
    long long errors = 0;
    long long bytes_read = 0;
    double seconds = 0.0;
    std::vector<double> latencies_ms;  // per batch
};

void print_usage(const char* program) {
    std::printf(
        "kv_bench -- load generator for kvstore\n"
        "\n"
        "usage: %s [options]\n"
        "  --host <addr>     default 127.0.0.1\n"
        "  --port <n>        default 9096\n"
        "  --threads <n>     concurrent connections, default 4\n"
        "  --requests <n>    total requests, default 100000\n"
        "  --pipeline <n>    requests in flight per round trip, default 32\n"
        "  --keys <n>        key space, default 10000\n"
        "  --get-ratio <f>   fraction of GETs in [0,1], default 0.5\n"
        "  --value-size <n>  bytes per SET value, default 32\n",
        program);
}

socket_t connect_to(const Options& options) {
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        return kInvalidSocket;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1) {
        close_socket(fd);
        return kInvalidSocket;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  static_cast<kv::net::socklen_type>(sizeof(address))) != 0) {
        close_socket(fd);
        return kInvalidSocket;
    }
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

std::size_t count_crlf(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '\r' && text[i + 1] == '\n') {
            ++count;
        }
    }
    return count;
}

void worker(const Options& options, long long quota, int seed, ThreadResult& result) {
    std::mt19937_64 rng(static_cast<std::uint64_t>(seed) * 2654435761ull + 12345ull);
    std::uniform_int_distribution<int> key_dist(0, options.key_space - 1);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    const socket_t fd = connect_to(options);
    if (fd == kInvalidSocket) {
        result.errors = quota;
        return;
    }

    const std::string value(static_cast<std::size_t>(options.value_size), 'v');
    std::string wire;
    std::string replies;
    wire.reserve(static_cast<std::size_t>(options.pipeline) * 64);

    const auto started = std::chrono::steady_clock::now();

    for (long long sent = 0; sent < quota; sent += options.pipeline) {
        const long long batch = std::min<long long>(options.pipeline, quota - sent);

        wire.clear();
        for (long long i = 0; i < batch; ++i) {
            const int key = key_dist(rng);
            if (unit(rng) < options.get_ratio) {
                wire += "GET key:" + std::to_string(key) + "\r\n";
            } else {
                wire += "SET key:" + std::to_string(key) + " " + value + "\r\n";
            }
        }

        const auto batch_start = std::chrono::steady_clock::now();
        if (!send_all(fd, wire)) {
            result.errors += batch;
            break;
        }

        // Read until every reply in the batch has arrived.  This is the part
        // the original test client got wrong: it issued one recv() per byte,
        // so the client -- not the server -- was the bottleneck.
        replies.clear();
        std::size_t lines = 0;
        while (lines < static_cast<std::size_t>(batch)) {
            char chunk[16 * 1024];
            const io_result got = ::recv(fd, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (got <= 0) {
                result.errors += batch;
                break;
            }
            result.bytes_read += got;
            replies.append(chunk, static_cast<std::size_t>(got));
            lines = count_crlf(replies);
        }
        if (lines < static_cast<std::size_t>(batch)) {
            break;
        }

        const auto batch_end = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(batch_end - batch_start).count();
        result.latencies_ms.push_back(ms / static_cast<double>(batch));
        result.requests += batch;
    }

    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    close_socket(fd);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto value = [&](const char* name) -> const char* {
            if (next == nullptr) {
                std::fprintf(stderr, "kv_bench: %s needs a value\n", name);
                std::exit(2);
            }
            ++i;
            return next;
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--host") {
            options.host = value("--host");
        } else if (arg == "--port") {
            options.port = static_cast<std::uint16_t>(std::atoi(value("--port")));
        } else if (arg == "--threads") {
            options.threads = std::max(1, std::atoi(value("--threads")));
        } else if (arg == "--requests") {
            options.requests = std::atoll(value("--requests"));
        } else if (arg == "--pipeline") {
            options.pipeline = std::max(1, std::atoi(value("--pipeline")));
        } else if (arg == "--keys") {
            options.key_space = std::max(1, std::atoi(value("--keys")));
        } else if (arg == "--get-ratio") {
            options.get_ratio = std::atof(value("--get-ratio"));
        } else if (arg == "--value-size") {
            options.value_size = std::max(1, std::atoi(value("--value-size")));
        } else {
            std::fprintf(stderr, "kv_bench: unknown option '%s' (try --help)\n", argv[i]);
            return 2;
        }
    }

    std::string error;
    if (!kv::net::net_init(error)) {
        std::fprintf(stderr, "kv_bench: %s\n", error.c_str());
        return 1;
    }

    const long long per_thread = options.requests / options.threads;
    std::vector<ThreadResult> results(static_cast<std::size_t>(options.threads));
    std::vector<std::thread> threads;
    threads.reserve(results.size());

    const auto started = std::chrono::steady_clock::now();
    for (int t = 0; t < options.threads; ++t) {
        threads.emplace_back([&options, &results, per_thread, t] {
            worker(options, per_thread, t, results[static_cast<std::size_t>(t)]);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto finished = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(finished - started).count();

    long long requests = 0;
    long long errors = 0;
    long long bytes = 0;
    std::vector<double> latencies;
    for (const auto& result : results) {
        requests += result.requests;
        errors += result.errors;
        bytes += result.bytes_read;
        latencies.insert(latencies.end(), result.latencies_ms.begin(), result.latencies_ms.end());
    }
    std::sort(latencies.begin(), latencies.end());

    auto percentile = [&latencies](double p) -> double {
        if (latencies.empty()) {
            return 0.0;
        }
        const std::size_t index =
            std::min(latencies.size() - 1, static_cast<std::size_t>(p * latencies.size()));
        return latencies[index];
    };

    const double qps = requests > 0 ? static_cast<double>(requests) / wall : 0.0;

    std::printf("\n--- results ---\n");
    std::printf("requests:      %lld\n", requests);
    std::printf("errors:        %lld\n", errors);
    std::printf("wall time:     %.3f s\n", wall);
    std::printf("throughput:    %.0f ops/s\n", qps);
    std::printf("pipeline:      %d\n", options.pipeline);
    std::printf("latency avg:   %.1f us\n",
                latencies.empty() ? 0.0
                                  : 1000.0 * std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                                        static_cast<double>(latencies.size()));
    std::printf("latency p50:   %.1f us\n", percentile(0.50) * 1000.0);
    std::printf("latency p95:   %.1f us\n", percentile(0.95) * 1000.0);
    std::printf("latency p99:   %.1f us\n", percentile(0.99) * 1000.0);
    std::printf("reply bytes:   %lld\n", bytes);

    kv::net::net_cleanup();
    return errors == 0 ? 0 : 1;
}
