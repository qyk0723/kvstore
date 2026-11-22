#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "kv/engine.hpp"
#include "kv/net/server.hpp"

namespace {

void print_usage(const char* program) {
    std::printf(
        "kvstore -- an in-memory key/value server (C++17 rewrite of the original C project)\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "  --bind <addr>          listen address (default 0.0.0.0)\n"
        "  --port <n>             listen port (default 9096)\n"
        "  --engine <name>        storage engine: %s (default hash)\n"
        "  --threads <n>          reactor threads, Linux only (default 1)\n"
        "  --shards <n>           store shards, each with its own lock (default 16)\n"
        "  --connections <n>      max concurrent connections (default 8192)\n"
        "  --db <path>            enable the write-ahead log at <path>\n"
        "  --fsync                fsync after every log write (slow, durable)\n"
        "  --help                 this message\n"
        "\n"
        "protocol (one command per line, CRLF terminated):\n"
        "  SET <key> <value>   GET <key>   DEL <key>   MOD <key> <value>\n"
        "  COUNT   KEYS [prefix]   FLUSH   PING   INFO   QUIT\n"
        "\n"
        "example:\n"
        "  %s --engine rbtree --port 9096\n"
        "  printf 'SET hello world\\r\\nGET hello\\r\\n' | nc 127.0.0.1 9096\n",
        program, kv::engine_names().c_str(), program);
}

bool parse_size(const char* text, std::size_t& out) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    kv::net::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        auto need_value = [&](const char* name) -> const char* {
            if (next == nullptr) {
                std::fprintf(stderr, "kvstore: %s needs a value\n", name);
                std::exit(2);
            }
            ++i;
            return next;
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--bind") {
            config.bind_addr = need_value("--bind");
        } else if (arg == "--port") {
            std::size_t value = 0;
            if (!parse_size(need_value("--port"), value) || value > 65535) {
                std::fprintf(stderr, "kvstore: invalid --port\n");
                return 2;
            }
            config.port = static_cast<std::uint16_t>(value);
        } else if (arg == "--engine") {
            config.engine = need_value("--engine");
        } else if (arg == "--threads") {
            const char* value = need_value("--threads");
            config.threads = std::atoi(value);
        } else if (arg == "--shards") {
            std::size_t value = 0;
            if (!parse_size(need_value("--shards"), value) || value == 0) {
                std::fprintf(stderr, "kvstore: invalid --shards\n");
                return 2;
            }
            config.shards = value;
        } else if (arg == "--connections") {
            std::size_t value = 0;
            if (!parse_size(need_value("--connections"), value) || value == 0) {
                std::fprintf(stderr, "kvstore: invalid --connections\n");
                return 2;
            }
            config.max_connections = value;
        } else if (arg == "--db") {
            config.db_path = need_value("--db");
        } else if (arg == "--fsync") {
            config.fsync_each_write = true;
        } else {
            std::fprintf(stderr, "kvstore: unknown option '%s' (try --help)\n", argv[i]);
            return 2;
        }
    }

    if (!kv::make_engine(config.engine)) {
        std::fprintf(stderr, "kvstore: unknown engine '%s' (available: %s)\n",
                     config.engine.c_str(), kv::engine_names().c_str());
        return 2;
    }

    std::string net_error;
    if (!kv::net::net_init(net_error)) {
        std::fprintf(stderr, "kvstore: %s\n", net_error.c_str());
        return 1;
    }

    int status = 0;
    {
        kv::net::Server server(config);
        status = server.run();
    }
    kv::net::net_cleanup();
    return status;
}
