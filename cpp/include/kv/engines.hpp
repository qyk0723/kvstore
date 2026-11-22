#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kv/engine.hpp"

namespace kv {

// ---------------------------------------------------------------------------
// ArrayEngine -- the naive baseline of the original project, kept on purpose.
// Linear scan over a flat vector: O(n) per operation.  It exists so the
// benchmark table has a "why data structures matter" row.
// ---------------------------------------------------------------------------
class ArrayEngine final : public Engine {
public:
    const char* name() const noexcept override { return "array"; }

    Status put(std::string_view key, std::string_view value) override;
    std::optional<std::string> get(std::string_view key) const override;
    Status erase(std::string_view key) override;
    Status update(std::string_view key, std::string_view value) override;
    std::size_t size() const noexcept override { return items_.size(); }
    void clear() override { items_.clear(); }
    void for_each(const VisitFn& visit) const override;

private:
    struct Entry {
        std::string key;
        std::string value;
    };
    std::vector<Entry> items_;
};

// ---------------------------------------------------------------------------
// HashEngine -- separate chaining with per-bucket contiguous storage.
//
// Differences from the original:
//   * FNV-1a instead of "sum of the bytes", which made every anagram collide;
//   * the load factor is maintained by rehashing instead of a fixed 1024 slots;
//   * erase actually walks the chain (the C version compared the bucket head
//     on every step, so it could only ever delete the first node).
// ---------------------------------------------------------------------------
class HashEngine final : public Engine {
public:
    explicit HashEngine(std::size_t initial_buckets = 1024);

    const char* name() const noexcept override { return "hash"; }

    Status put(std::string_view key, std::string_view value) override;
    std::optional<std::string> get(std::string_view key) const override;
    Status erase(std::string_view key) override;
    Status update(std::string_view key, std::string_view value) override;
    std::size_t size() const noexcept override { return size_; }
    void clear() override;
    void for_each(const VisitFn& visit) const override;

    std::size_t bucket_count() const noexcept { return buckets_.size(); }
    double load_factor() const noexcept {
        return buckets_.empty() ? 0.0
                                : static_cast<double>(size_) / static_cast<double>(buckets_.size());
    }

    static std::uint64_t fnv1a(std::string_view key) noexcept;

private:
    struct Node {
        std::string key;
        std::string value;
    };

    std::size_t bucket_of(std::string_view key) const noexcept {
        return static_cast<std::size_t>(fnv1a(key)) & (buckets_.size() - 1);
    }
    Node* find_node(std::string_view key);
    const Node* find_node(std::string_view key) const;
    void reserve_for(std::size_t wanted);
    void rehash(std::size_t new_size);

    std::vector<std::vector<Node>> buckets_;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// UnorderedEngine -- thin wrapper over std::unordered_map.  Not interesting
// as a data structure, but it is a useful "the standard library already does
// this well" baseline next to the hand-written hash table.
// ---------------------------------------------------------------------------
class UnorderedEngine final : public Engine {
public:
    const char* name() const noexcept override { return "unordered"; }

    Status put(std::string_view key, std::string_view value) override;
    std::optional<std::string> get(std::string_view key) const override;
    Status erase(std::string_view key) override;
    Status update(std::string_view key, std::string_view value) override;
    std::size_t size() const noexcept override { return map_.size(); }
    void clear() override { map_.clear(); }
    void for_each(const VisitFn& visit) const override;

private:
    std::unordered_map<std::string, std::string> map_;
};

// ---------------------------------------------------------------------------
// RBTreeEngine -- clocked red-black tree.
//
// The single most important change from the C version is the sentinel: `nil_`
// is an *embedded* node whose left/right/parent all point at itself, and it is
// black.  The C code heap-allocated the sentinel and only initialised `key`
// and `color`, so the delete fix-up dereferenced garbage pointers and crashed
// at random.  Making the sentinel a member also removes the malloc/free pair
// and the "is the sentinel NULL?" special cases.
//
// Nodes are carved out of a std::pmr::unsynchronized_pool_resource instead of
// calling the global allocator per node.  Each shard owns its engine, so the
// pool is only ever touched by one thread at a time.
// ---------------------------------------------------------------------------
class RBTreeEngine final : public Engine {
public:
    RBTreeEngine();
    ~RBTreeEngine() override;

    RBTreeEngine(const RBTreeEngine&) = delete;
    RBTreeEngine& operator=(const RBTreeEngine&) = delete;

    const char* name() const noexcept override { return "rbtree"; }

    Status put(std::string_view key, std::string_view value) override;
    std::optional<std::string> get(std::string_view key) const override;
    Status erase(std::string_view key) override;
    Status update(std::string_view key, std::string_view value) override;
    std::size_t size() const noexcept override { return size_; }
    void clear() override;
    void for_each(const VisitFn& visit) const override;

    // O(n) consistency check used by the tests: verifies BST ordering plus the
    // five red-black invariants and that the stored size matches the node
    // count.  Returns false on the first violation.
    bool check_invariants() const;

    int height() const;

private:
    enum class Color : std::uint8_t { Red, Black };

    struct Node {
        std::string key;
        std::string value;
        Node* left = nullptr;
        Node* right = nullptr;
        Node* parent = nullptr;
        Color color = Color::Red;
    };

    Node* nil() noexcept { return &nil_; }
    const Node* nil() const noexcept { return &nil_; }

    Node* allocate_node();
    void release_node(Node* node);
    void destroy_subtree(Node* node);

    Node* find_node(std::string_view key) const;
    void walk(const Node* node, const VisitFn& visit) const;

    void left_rotate(Node* x);
    void right_rotate(Node* x);
    void insert_node(Node* z);
    void insert_fixup(Node* z);
    void transplant(Node* u, Node* v);
    void erase_node(Node* z);
    void erase_fixup(Node* x);
    Node* minimum(Node* node) const;

    static bool is_red(const Node* n) noexcept {
        return n != nullptr && n->color == Color::Red;
    }
    static bool is_black(const Node* n) noexcept {
        return n == nullptr || n->color == Color::Black;
    }

    std::pmr::unsynchronized_pool_resource pool_;
    Node nil_;
    Node* root_ = nullptr;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// SkipListEngine -- the structure the original only sketched (it used int
// keys, had no value storage and was never wired into the command layer).
// Here it is a real string-keyed map with geometric level promotion.
// ---------------------------------------------------------------------------
class SkipListEngine final : public Engine {
public:
    static constexpr int kMaxLevel = 24;

    explicit SkipListEngine(std::uint64_t seed = 0x9e3779b97f4a7c15ull);
    ~SkipListEngine() override;

    SkipListEngine(const SkipListEngine&) = delete;
    SkipListEngine& operator=(const SkipListEngine&) = delete;

    const char* name() const noexcept override { return "skiplist"; }

    Status put(std::string_view key, std::string_view value) override;
    std::optional<std::string> get(std::string_view key) const override;
    Status erase(std::string_view key) override;
    Status update(std::string_view key, std::string_view value) override;
    std::size_t size() const noexcept override { return size_; }
    void clear() override;
    void for_each(const VisitFn& visit) const override;

    int level() const noexcept { return level_; }
    bool check_invariants() const;

private:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node*> next;
    };

    Node* create_node(std::string_view key, std::string_view value, int level);
    void destroy_node(Node* node);
    int random_level();
    Node* find_node(std::string_view key) const;

    std::uint64_t seed_;
    Node* head_;
    int level_ = 0;
    std::size_t size_ = 0;
};

}  // namespace kv
