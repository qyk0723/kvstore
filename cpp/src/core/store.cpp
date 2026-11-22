#include "kv/store.hpp"

#include <stdexcept>
#include <utility>

#include "kv/engines.hpp"
#include "kv/wal.hpp"

namespace kv {

namespace {
std::size_t shard_index(std::string_view key, std::size_t shard_count) noexcept {
    // FNV-1a, same function the hash engine uses.
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return static_cast<std::size_t>(hash) % shard_count;
}
}  // namespace

ShardedStore::ShardedStore(std::string engine_name, std::size_t shard_count, Wal* wal)
    : engine_name_(std::move(engine_name)), wal_(wal) {
    if (shard_count == 0) {
        shard_count = 1;
    }
    shards_.resize(shard_count);
    for (auto& shard : shards_) {
        shard.engine = make_engine(engine_name_);
        if (!shard.engine) {
            throw std::invalid_argument("unknown engine: " + engine_name_);
        }
    }
}

ShardedStore::~ShardedStore() = default;

std::size_t ShardedStore::shard_of(std::string_view key) const noexcept {
    return shard_index(key, shards_.size());
}

void ShardedStore::log_put(std::string_view key, std::string_view value) {
    if (wal_ == nullptr) {
        return;
    }
    if (!wal_->append(Wal::Op::Put, key, value)) {
        wal_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ShardedStore::log_erase(std::string_view key) {
    if (wal_ == nullptr) {
        return;
    }
    if (!wal_->append(Wal::Op::Erase, key)) {
        wal_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

Status ShardedStore::put(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    Shard& shard = shards_[shard_of(key)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    // Log before applying: that ordering is what makes the log a *write-ahead*
    // log.  The append happens under the shard lock, so two writes to the same
    // key can never be reordered in the log relative to the database.
    log_put(key, value);
    return shard.engine->put(key, value);
}

std::optional<std::string> ShardedStore::get(std::string_view key) const {
    if (key.empty()) {
        return std::nullopt;
    }
    const Shard& shard = shards_[shard_of(key)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    return shard.engine->get(key);
}

Status ShardedStore::erase(std::string_view key) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    Shard& shard = shards_[shard_of(key)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    // Only log a deletion that actually removes something, otherwise replay
    // would be a no-op anyway -- this just keeps the log smaller.
    const bool present = shard.engine->get(key).has_value();
    if (!present) {
        return Status::NotFound;
    }
    log_erase(key);
    return shard.engine->erase(key);
}

Status ShardedStore::update(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    Shard& shard = shards_[shard_of(key)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    if (!shard.engine->get(key).has_value()) {
        return Status::NotFound;
    }
    log_put(key, value);  // an update is recorded as a full write
    return shard.engine->update(key, value);
}

std::size_t ShardedStore::size() const noexcept {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        total += shard.engine->size();
    }
    return total;
}

void ShardedStore::clear() {
    for (auto& shard : shards_) {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        shard.engine->clear();
    }
}

void ShardedStore::for_each(const VisitFn& visit) const {
    for (const auto& shard : shards_) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        shard.engine->for_each(visit);
    }
}

std::vector<std::size_t> ShardedStore::shard_sizes() const {
    std::vector<std::size_t> sizes;
    sizes.reserve(shards_.size());
    for (const auto& shard : shards_) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        sizes.push_back(shard.engine->size());
    }
    return sizes;
}

}  // namespace kv
