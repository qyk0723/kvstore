#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "kv/engine.hpp"

namespace kv {

class Wal;

// A key/value store split into N independent shards.
//
// Why sharding: the original server owned three *global* engine instances and
// the whole process was single-threaded, so a single mutex around the store
// would serialise every request no matter how many reactor threads existed.
// Hashing the key to a shard and giving each shard its own std::shared_mutex
// means readers on different shards never contend, and readers of the same
// shard still run concurrently.
class ShardedStore {
public:
    // `proto` is a prototype engine: the store asks it for its name and then
    // creates one independent instance per shard through `make_engine`.
    ShardedStore(std::string engine_name, std::size_t shard_count, Wal* wal = nullptr);
    ~ShardedStore();

    ShardedStore(const ShardedStore&) = delete;
    ShardedStore& operator=(const ShardedStore&) = delete;

    Status put(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key) const;
    Status erase(std::string_view key);
    Status update(std::string_view key, std::string_view value);

    std::size_t size() const noexcept;
    void clear();

    // Visits every key/value pair, one shard at a time.  Holds each shard's
    // shared lock only while that shard is being scanned, so the result is
    // consistent per shard but not a global snapshot.
    void for_each(const VisitFn& visit) const;

    std::size_t shard_count() const noexcept { return shards_.size(); }
    std::size_t shard_of(std::string_view key) const noexcept;
    std::vector<std::size_t> shard_sizes() const;
    const std::string& engine_name() const noexcept { return engine_name_; }

    std::uint64_t wal_failures() const noexcept { return wal_failures_.load(std::memory_order_relaxed); }

    // WAL attachment is deferred so recovery can replay into a store whose
    // log is not yet active (otherwise replayed writes would be re-logged).
    void set_wal(Wal* wal) noexcept { wal_ = wal; }

private:
    struct Shard {
        std::unique_ptr<Engine> engine;
        mutable std::shared_mutex mutex;
    };

    void log_put(std::string_view key, std::string_view value);
    void log_erase(std::string_view key);

    std::string engine_name_;
    std::deque<Shard> shards_;
    Wal* wal_ = nullptr;
    std::atomic<std::uint64_t> wal_failures_{0};
};

}  // namespace kv
