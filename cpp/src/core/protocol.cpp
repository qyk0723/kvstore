#include "kv/protocol.hpp"

#include <array>

namespace kv {

namespace {

struct CommandEntry {
    std::string_view name;
    Command cmd;
};

constexpr std::array<CommandEntry, 10> kCommands{{
    {"SET", Command::Set},
    {"GET", Command::Get},
    {"DEL", Command::Del},
    {"MOD", Command::Mod},
    {"COUNT", Command::Count},
    {"KEYS", Command::Keys},
    {"PING", Command::Ping},
    {"INFO", Command::Info},
    {"FLUSH", Command::Flush},
    {"QUIT", Command::Quit},
}};

constexpr char to_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

}  // namespace

const char* command_name(Command cmd) noexcept {
    switch (cmd) {
        case Command::Set: return "SET";
        case Command::Get: return "GET";
        case Command::Del: return "DEL";
        case Command::Mod: return "MOD";
        case Command::Count: return "COUNT";
        case Command::Keys: return "KEYS";
        case Command::Ping: return "PING";
        case Command::Info: return "INFO";
        case Command::Flush: return "FLUSH";
        case Command::Quit: return "QUIT";
        case Command::Unknown: break;
    }
    return "?";
}

RequestParser::Result RequestParser::next(Request& out) {
    for (;;) {
        const std::string_view data = buffer_.readable();
        if (data.empty()) {
            return Result::Incomplete;
        }

        const std::size_t newline = data.find('\n');
        if (newline == std::string_view::npos) {
            // No terminator yet.  Bound the wait so a client that never sends
            // a newline cannot make us buffer forever.
            if (data.size() > max_line_) {
                error_ = "request line too long";
                buffer_.clear();
                return Result::Error;
            }
            return Result::Incomplete;
        }
        if (newline > max_line_) {
            error_ = "request line too long";
            buffer_.clear();
            return Result::Error;
        }

        std::string_view line = data.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Tokenise before consuming: `line` points into the buffer, and
        // consume() may reset it.
        out.args.clear();
        out.cmd = Command::Unknown;

        std::size_t pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && is_space(line[pos])) {
                ++pos;
            }
            if (pos >= line.size()) {
                break;
            }
            const std::size_t start = pos;
            while (pos < line.size() && !is_space(line[pos])) {
                ++pos;
            }
            if (out.args.size() >= max_args_) {
                error_ = "too many arguments";
                return Result::Error;
            }
            out.args.emplace_back(line.substr(start, pos - start));
        }

        buffer_.consume(newline + 1);

        if (out.args.empty()) {
            continue;  // empty line, try the next one
        }

        const std::string_view word = out.args[0];
        for (const auto& entry : kCommands) {
            if (entry.name.size() != word.size()) {
                continue;
            }
            bool match = true;
            for (std::size_t i = 0; i < word.size(); ++i) {
                if (to_upper(word[i]) != entry.name[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out.cmd = entry.cmd;
                break;
            }
        }
        // An unknown command is *not* a parser error: the handler answers with
        // "ERROR:unknown command" and keeps the connection open.  The original
        // server called assert(0) here, so one malformed request killed the
        // whole process.
        return Result::Ok;
    }
}

}  // namespace kv
