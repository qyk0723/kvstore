#pragma once

#include <cstdint>

namespace kv {

// Uniform result type for every engine operation.  The original C project
// encoded this in int return codes with different meanings per engine
// (0 == success, >0 == missing, -1 == error, and the array engine even
// returned its array index), which made the adapter layer unreadable.
enum class Status : std::uint8_t {
    Ok = 0,
    NotFound,
    InvalidArgument,
    Error,
};

inline const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotFound: return "not found";
        case Status::InvalidArgument: return "invalid argument";
        case Status::Error: return "error";
    }
    return "unknown";
}

}  // namespace kv
