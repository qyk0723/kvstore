#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kv/buffer.hpp"

namespace kv {

enum class Command : std::uint8_t {
    Set,
    Get,
    Del,
    Mod,
    Count,
    Keys,
    Ping,
    Info,
    Flush,
    Quit,
    Unknown,
};

const char* command_name(Command cmd) noexcept;

struct Request {
    Command cmd = Command::Unknown;
    std::vector<std::string> args;  // args[0] is the command word as sent
};

// Incremental, line-framed request parser.
//
// The wire format stays compatible with the original C server ("SET key value"
// terminated by CRLF) but the parser fixes three real defects:
//
//   1. Requests are framed on CRLF, so a command split across TCP segments is
//      buffered instead of being executed half-parsed (the C version had no
//      reassembly at all).
//   2. The line terminator is stripped from the last argument.  The C version
//      split on spaces only, so every stored value kept a trailing "\r\n".
//   3. An over-long line is reported as an error instead of growing forever,
//      and the argument count is bounded.
//
// next() returns Incomplete when more bytes are needed, Ok when a request was
// produced, and Error when the stream must be rejected (the caller usually
// replies with the error text and closes the connection).
class RequestParser {
public:
    enum class Result { Ok, Incomplete, Error };

    explicit RequestParser(std::size_t max_line = 64 * 1024, std::size_t max_args = 64)
        : max_line_(max_line), max_args_(max_args) {}

    void append(std::string_view bytes) { buffer_.append(bytes); }
    void append(const char* data, std::size_t n) { buffer_.append({data, n}); }

    // Zero-copy read path: receive directly into the parser's own buffer, so
    // the socket read and the framing state can never disagree.
    char* prepare_read(std::size_t n) {
        buffer_.ensure_writable(n);
        return buffer_.begin_write();
    }
    void commit_read(std::size_t n) noexcept { buffer_.commit(n); }

    Result next(Request& out);

    void clear() noexcept {
        buffer_.clear();
        error_.clear();
    }

    std::size_t buffered() const noexcept { return buffer_.size(); }
    const std::string& last_error() const noexcept { return error_; }

private:
    Buffer buffer_;
    std::size_t max_line_;
    std::size_t max_args_;
    std::string error_;
};

}  // namespace kv
