#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace kv {

std::uint32_t crc32(const void* data, std::size_t len) noexcept;

// Append-only write-ahead log.
//
// Record layout (all integers little endian):
//
//   +------------+------------+--------------------------------------+
//   | len: u32   | crc32: u32 | payload[len]                          |
//   +------------+------------+--------------------------------------+
//
//   payload = op: u8 | klen: u32 | key | vlen: u32 | value
//
// The CRC covers the payload only, so a torn tail written by a crash is
// detected and recovery stops cleanly at the last intact record instead of
// replaying garbage.  This is the piece the original project did not have at
// all: it was pure in-memory and lost everything on restart.
class Wal {
public:
    enum class Op : std::uint8_t { Put = 1, Erase = 2 };

    // Opens (creating if needed) `path` for appending.  On failure returns
    // nullptr and fills `err`.
    static std::unique_ptr<Wal> open(const std::string& path, bool fsync_each_write,
                                     std::string& err);

    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    bool append(Op op, std::string_view key, std::string_view value = {});

    // Streams every intact record through `fn`.  Returns the number of records
    // replayed.  A truncated or corrupt trailing record stops replay with a
    // warning rather than an error, which is what a crash looks like.
    std::size_t replay(const std::function<void(Op, std::string_view, std::string_view)>& fn,
                       std::string& err);

    bool flush() noexcept;
    bool sync() noexcept;

    std::uint64_t bytes_written() const noexcept { return bytes_.load(std::memory_order_relaxed); }
    std::uint64_t records_written() const noexcept { return records_.load(std::memory_order_relaxed); }
    const std::string& path() const noexcept { return path_; }

private:
    Wal(std::FILE* fp, std::string path, bool fsync_each_write);

    mutable std::mutex mutex_;
    std::FILE* fp_ = nullptr;
    std::string path_;
    bool fsync_each_write_ = false;
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> records_{0};
};

}  // namespace kv
