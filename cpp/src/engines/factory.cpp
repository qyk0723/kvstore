#include "kv/engines.hpp"

namespace kv {

std::unique_ptr<Engine> make_engine(std::string_view name) {
    if (name == "array") {
        return std::make_unique<ArrayEngine>();
    }
    if (name == "hash") {
        return std::make_unique<HashEngine>();
    }
    if (name == "unordered") {
        return std::make_unique<UnorderedEngine>();
    }
    if (name == "rbtree") {
        return std::make_unique<RBTreeEngine>();
    }
    if (name == "skiplist") {
        return std::make_unique<SkipListEngine>();
    }
    return nullptr;
}

std::string engine_names() { return "array, hash, unordered, rbtree, skiplist"; }

}  // namespace kv
