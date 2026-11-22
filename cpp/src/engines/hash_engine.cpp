#include "kv/engines.hpp"

#include <algorithm>
#include <limits>

namespace kv {

namespace {
constexpr std::size_t kMinBuckets = 16;

std::size_t round_up_pow2(std::size_t n) {
    std::size_t p = kMinBuckets;
    while (p < n) {
        p <<= 1;
    }
    return p;
}
}  // namespace

HashEngine::HashEngine(std::size_t initial_buckets) {
    buckets_.resize(round_up_pow2(initial_buckets));
}

// FNV-1a.  The C version summed the bytes of the key, which collides for every
// anagram ("abc", "bca", "cab" hashed to the same bucket) and clusters badly
// for sequential keys.
std::uint64_t HashEngine::fnv1a(std::string_view key) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

HashEngine::Node* HashEngine::find_node(std::string_view key) {
    for (auto& node : buckets_[bucket_of(key)]) {
        if (equals(node.key, key)) {
            return &node;
        }
    }
    return nullptr;
}

const HashEngine::Node* HashEngine::find_node(std::string_view key) const {
    for (const auto& node : buckets_[bucket_of(key)]) {
        if (equals(node.key, key)) {
            return &node;
        }
    }
    return nullptr;
}

void HashEngine::rehash(std::size_t new_size) {
    std::vector<std::vector<Node>> next(new_size);
    for (auto& chain : buckets_) {
        for (auto& node : chain) {
            std::size_t idx = static_cast<std::size_t>(fnv1a(node.key)) & (new_size - 1);
            next[idx].push_back(std::move(node));
        }
        chain.clear();
    }
    buckets_.swap(next);
}

void HashEngine::reserve_for(std::size_t wanted) {
    // Keep the load factor at or below 1.0.  Because the bucket count is a
    // power of two, `wanted <= buckets` is exactly the load-factor test.
    if (wanted <= buckets_.size()) {
        return;
    }
    rehash(buckets_.size() * 2);
}

Status HashEngine::put(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    if (Node* node = find_node(key)) {
        node->value.assign(value);
        return Status::Ok;
    }
    reserve_for(size_ + 1);
    // Re-fetch the chain: reserve_for() may have reallocated buckets_.
    buckets_[bucket_of(key)].push_back(Node{std::string(key), std::string(value)});
    ++size_;
    return Status::Ok;
}

std::optional<std::string> HashEngine::get(std::string_view key) const {
    const Node* node = find_node(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->value;
}

Status HashEngine::erase(std::string_view key) {
    // Walks the chain properly.  legacy/kvstore_hash.c compared the *head*
    // key on every iteration, so anything past the first node was treated as
    // missing and could never be deleted.
    auto& chain = buckets_[bucket_of(key)];
    for (auto it = chain.begin(); it != chain.end(); ++it) {
        if (equals(it->key, key)) {
            chain.erase(it);
            --size_;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

Status HashEngine::update(std::string_view key, std::string_view value) {
    Node* node = find_node(key);
    if (node == nullptr) {
        return Status::NotFound;
    }
    node->value.assign(value);
    return Status::Ok;
}

void HashEngine::clear() {
    for (auto& chain : buckets_) {
        chain.clear();
    }
    size_ = 0;
}

void HashEngine::for_each(const VisitFn& visit) const {
    for (const auto& chain : buckets_) {
        for (const auto& node : chain) {
            visit(node.key, node.value);
        }
    }
}

}  // namespace kv
