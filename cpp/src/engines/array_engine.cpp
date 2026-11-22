#include "kv/engines.hpp"

namespace kv {

// The array engine is intentionally the slow one: it is the "before" row of
// the benchmark, and it is what the original project shipped for SET/GET.
//
// Note the three defects fixed here relative to legacy/kvstore_array.c:
//   * SET on an existing key overwrites instead of appending a duplicate, so
//     GET can no longer return a stale value;
//   * erase() moves the last element into the freed slot, so the table never
//     loses its tail entry;
//   * destroy() actually releases the table (the C condition was inverted).

Status ArrayEngine::put(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    for (auto& entry : items_) {
        if (equals(entry.key, key)) {
            entry.value.assign(value);
            return Status::Ok;
        }
    }
    items_.push_back(Entry{std::string(key), std::string(value)});
    return Status::Ok;
}

std::optional<std::string> ArrayEngine::get(std::string_view key) const {
    for (const auto& entry : items_) {
        if (equals(entry.key, key)) {
            return entry.value;
        }
    }
    return std::nullopt;
}

Status ArrayEngine::erase(std::string_view key) {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!equals(items_[i].key, key)) {
            continue;
        }
        // O(1) removal: the tail entry takes the hole.  Order is not part of
        // the contract for a key/value map.
        if (i + 1 != items_.size()) {
            items_[i] = std::move(items_.back());
        }
        items_.pop_back();
        return Status::Ok;
    }
    return Status::NotFound;
}

Status ArrayEngine::update(std::string_view key, std::string_view value) {
    for (auto& entry : items_) {
        if (equals(entry.key, key)) {
            entry.value.assign(value);
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

void ArrayEngine::for_each(const VisitFn& visit) const {
    for (const auto& entry : items_) {
        visit(entry.key, entry.value);
    }
}

}  // namespace kv
