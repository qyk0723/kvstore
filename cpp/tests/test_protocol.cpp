#include "test_framework.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "kv/protocol.hpp"

namespace {

using kv::Command;
using kv::Request;
using kv::RequestParser;

std::vector<Request> drain(RequestParser& parser) {
    std::vector<Request> requests;
    Request request;
    for (;;) {
        const auto result = parser.next(request);
        if (result != RequestParser::Result::Ok) {
            break;
        }
        requests.push_back(request);
    }
    return requests;
}

}  // namespace

KV_TEST(protocol_parses_one_request) {
    RequestParser parser;
    parser.append("SET alpha 1\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Set);
    KV_CHECK_EQ(request.args.size(), 3);
    KV_CHECK_STR(request.args[0], "SET");
    KV_CHECK_STR(request.args[1], "alpha");
    KV_CHECK_STR(request.args[2], "1");
    KV_CHECK_EQ(parser.buffered(), 0);
}

// The whole point of the rewrite: a request split across TCP segments must be
// reassembled, not executed half-parsed.
KV_TEST(protocol_reassembles_split_request) {
    RequestParser parser;
    Request request;

    parser.append("GET al");
    KV_CHECK(parser.next(request) == RequestParser::Result::Incomplete);
    parser.append("pha\r");
    KV_CHECK(parser.next(request) == RequestParser::Result::Incomplete);
    parser.append("\n");
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Get);
    KV_CHECK_EQ(request.args.size(), 2);
    KV_CHECK_STR(request.args[1], "alpha");
}

KV_TEST(protocol_reassembles_byte_by_byte) {
    RequestParser parser;
    Request request;
    const std::string wire = "SET k v\r\n";
    for (std::size_t i = 0; i < wire.size(); ++i) {
        parser.append(&wire[i], 1);
        const auto result = parser.next(request);
        if (i + 1 < wire.size()) {
            KV_CHECK(result == RequestParser::Result::Incomplete);
        } else {
            KV_CHECK(result == RequestParser::Result::Ok);
        }
    }
    KV_CHECK_STR(request.args[2], "v");
}

// Pipelining: three commands arriving in a single read.
KV_TEST(protocol_pipelines_multiple_requests) {
    RequestParser parser;
    parser.append("SET a 1\r\nSET b 2\r\nGET a\r\n");
    const auto requests = drain(parser);
    KV_CHECK_EQ(requests.size(), 3);
    if (requests.size() == 3) {
        KV_CHECK(requests[0].cmd == Command::Set);
        KV_CHECK(requests[1].cmd == Command::Set);
        KV_CHECK(requests[2].cmd == Command::Get);
    }
    KV_CHECK_EQ(parser.buffered(), 0);
}

// legacy/kvstore.c split on spaces only, so every stored value kept the
// trailing CRLF.  The terminator must never reach an argument.
KV_TEST(protocol_strips_crlf_from_last_argument) {
    RequestParser parser;
    parser.append("SET k value\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK_STR(request.args[2], "value");
    KV_CHECK(request.args[2].find('\r') == std::string::npos);
    KV_CHECK(request.args[2].find('\n') == std::string::npos);
}

KV_TEST(protocol_accepts_bare_lf) {  // nc without -C, for example
    RequestParser parser;
    parser.append("PING\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Ping);
}

KV_TEST(protocol_is_case_insensitive) {
    RequestParser parser;
    parser.append("set a 1\r\nGeT a\r\ncount\r\n");
    const auto requests = drain(parser);
    KV_CHECK_EQ(requests.size(), 3);
    if (requests.size() == 3) {
        KV_CHECK(requests[0].cmd == Command::Set);
        KV_CHECK(requests[1].cmd == Command::Get);
        KV_CHECK(requests[2].cmd == Command::Count);
    }
}

KV_TEST(protocol_accepts_tabs_and_extra_spaces) {
    RequestParser parser;
    parser.append("  SET\t\tk   v  \r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK_EQ(request.args.size(), 3);
    KV_CHECK_STR(request.args[1], "k");
    KV_CHECK_STR(request.args[2], "v");
}

KV_TEST(protocol_skips_blank_lines) {
    RequestParser parser;
    parser.append("\r\n\r\n   \r\nPING\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Ping);
}

KV_TEST(protocol_accepts_carriage_return_inside_line) {
    // A CR in the middle (not followed by LF) is just part of the token.
    RequestParser parser;
    parser.append("SET a b\rc\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK_EQ(request.args.size(), 3);
    KV_CHECK_STR(request.args[2], "b\rc");
}

// An unknown command is a normal (non-fatal) result: the handler replies with
// an error.  The C server called assert(0) and died here.
KV_TEST(protocol_unknown_command_is_not_an_error) {
    RequestParser parser;
    parser.append("FROBNICATE now\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Unknown);
    KV_CHECK_EQ(request.args.size(), 2);
}

KV_TEST(protocol_rejects_overlong_line) {
    RequestParser parser(64);  // 64-byte limit for the test
    parser.append("SET k " + std::string(200, 'x'));  // no newline yet
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Error);
    KV_CHECK(!parser.last_error().empty());
    KV_CHECK_EQ(parser.buffered(), 0);  // the bad line is dropped
}

KV_TEST(protocol_rejects_too_many_arguments) {
    RequestParser parser(4096, 4);
    parser.append("SET a b c d e f\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Error);
}

KV_TEST(protocol_keeps_missing_arguments_missing) {
    // Arity checking belongs to the command handler; the parser must simply
    // report what it saw instead of reading past the end of the buffer.
    RequestParser parser;
    parser.append("GET\r\n");
    Request request;
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Get);
    KV_CHECK_EQ(request.args.size(), 1);
}

KV_TEST(protocol_huge_pipeline_roundtrip) {
    RequestParser parser;
    std::string wire;
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) {
        wire += "SET k" + std::to_string(i) + " v\r\n";
    }
    // Feed it in 7-byte chunks to exercise buffer compaction.
    std::size_t offset = 0;
    int produced = 0;
    Request request;
    while (offset < wire.size()) {
        const std::size_t chunk = std::min<std::size_t>(7, wire.size() - offset);
        parser.append(wire.data() + offset, chunk);
        offset += chunk;
        while (parser.next(request) == RequestParser::Result::Ok) {
            ++produced;
        }
    }
    KV_CHECK_EQ(produced, kCount);
    KV_CHECK_EQ(parser.buffered(), 0);
}

KV_TEST(protocol_parses_keys_with_optional_prefix) {
    RequestParser parser;
    parser.append("KEYS\r\nkeys user:\r\n");
    Request request;

    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Keys);
    KV_CHECK_EQ(request.args.size(), 1);

    // Lowercase must still map to the same command.
    KV_CHECK(parser.next(request) == RequestParser::Result::Ok);
    KV_CHECK(request.cmd == Command::Keys);
    KV_CHECK_EQ(request.args.size(), 2);
    KV_CHECK_STR(request.args[1], "user:");
}
