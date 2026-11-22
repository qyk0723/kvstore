#include "kv/engines.hpp"

namespace kv {

// The original legacy/kvstore_skiplist.c had integer-only keys, stored no
// values, and was never wired into the command table.  This is a complete
// string-keyed map: geometric level promotion, O(log n) search/insert/erase,
// and a level that shrinks again when the top list empties.

SkipListEngine::SkipListEngine(std::uint64_t seed) : seed_(seed) {
    head_ = create_node(std::string_view{}, std::string_view{}, kMaxLevel);
    level_ = 0;
}

SkipListEngine::~SkipListEngine() {
    clear();
    destroy_node(head_);
}

SkipListEngine::Node* SkipListEngine::create_node(std::string_view key, std::string_view value,
                                                  int level) {
    Node* node = new Node();
    node->key.assign(key);
    node->value.assign(value);
    node->next.assign(static_cast<std::size_t>(level) + 1, nullptr);
    return node;
}

void SkipListEngine::destroy_node(Node* node) { delete node; }

// splitmix64: good avalanche on every bit, so an unlucky seed cannot bias the
// level distribution the way `rand() % 2` can.
int SkipListEngine::random_level() {
    std::uint64_t z = (seed_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;

    int level = 0;
    while (level < kMaxLevel && (z & 1u) != 0) {
        ++level;
        z >>= 1;
    }
    return level;
}

SkipListEngine::Node* SkipListEngine::find_node(std::string_view key) const {
    Node* cursor = head_;
    for (int i = level_; i >= 0; --i) {
        while (cursor->next[i] != nullptr && std::string_view(cursor->next[i]->key) < key) {
            cursor = cursor->next[i];
        }
    }
    cursor = cursor->next[0];
    if (cursor != nullptr && equals(cursor->key, key)) {
        return cursor;
    }
    return nullptr;
}

Status SkipListEngine::put(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }

    Node* update[kMaxLevel + 1];
    Node* cursor = head_;
    for (int i = level_; i >= 0; --i) {
        while (cursor->next[i] != nullptr && std::string_view(cursor->next[i]->key) < key) {
            cursor = cursor->next[i];
        }
        update[i] = cursor;
    }

    cursor = cursor->next[0];
    if (cursor != nullptr && equals(cursor->key, key)) {
        cursor->value.assign(value);
        return Status::Ok;
    }

    const int new_level = random_level();
    if (new_level > level_) {
        for (int i = level_ + 1; i <= new_level; ++i) {
            update[i] = head_;
        }
        level_ = new_level;
    }

    Node* node = create_node(key, value, new_level);
    for (int i = 0; i <= new_level; ++i) {
        node->next[i] = update[i]->next[i];
        update[i]->next[i] = node;
    }
    ++size_;
    return Status::Ok;
}

std::optional<std::string> SkipListEngine::get(std::string_view key) const {
    const Node* node = find_node(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->value;
}

Status SkipListEngine::erase(std::string_view key) {
    Node* update[kMaxLevel + 1];
    Node* cursor = head_;
    for (int i = level_; i >= 0; --i) {
        while (cursor->next[i] != nullptr && std::string_view(cursor->next[i]->key) < key) {
            cursor = cursor->next[i];
        }
        update[i] = cursor;
    }

    cursor = cursor->next[0];
    if (cursor == nullptr || !equals(cursor->key, key)) {
        return Status::NotFound;
    }

    for (int i = 0; i <= level_; ++i) {
        if (update[i]->next[i] != cursor) {
            break;  // the node does not appear on this level
        }
        update[i]->next[i] = cursor->next[i];
    }
    destroy_node(cursor);
    --size_;

    while (level_ > 0 && head_->next[level_] == nullptr) {
        --level_;
    }
    return Status::Ok;
}

Status SkipListEngine::update(std::string_view key, std::string_view value) {
    Node* node = find_node(key);
    if (node == nullptr) {
        return Status::NotFound;
    }
    node->value.assign(value);
    return Status::Ok;
}

void SkipListEngine::clear() {
    Node* node = head_->next[0];
    while (node != nullptr) {
        Node* next = node->next[0];
        destroy_node(node);
        node = next;
    }
    for (auto& link : head_->next) {
        link = nullptr;
    }
    level_ = 0;
    size_ = 0;
}

void SkipListEngine::for_each(const VisitFn& visit) const {
    for (const Node* node = head_->next[0]; node != nullptr; node = node->next[0]) {
        visit(node->key, node->value);
    }
}

bool SkipListEngine::check_invariants() const {
    // Every level must be sorted, and each higher level must be a subsequence
    // of level 0 (guaranteed by construction, checked here anyway).
    for (int i = 0; i <= level_; ++i) {
        std::string_view previous;
        bool first = true;
        for (Node* node = head_->next[i]; node != nullptr; node = node->next[i]) {
            const std::string_view key = node->key;
            if (!first && !(previous < key)) {
                return false;
            }
            previous = key;
            first = false;
        }
    }
    if (head_->next[0] == nullptr && level_ != 0) {
        return false;
    }

    std::size_t counted = 0;
    for (Node* node = head_->next[0]; node != nullptr; node = node->next[0]) {
        ++counted;
    }
    if (counted != size_) {
        return false;
    }
    for (int i = level_ + 1; i <= kMaxLevel; ++i) {
        if (head_->next[i] != nullptr) {
            return false;
        }
    }
    return true;
}

}  // namespace kv
