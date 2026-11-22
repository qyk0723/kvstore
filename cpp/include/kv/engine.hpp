#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "kv/status.hpp"

namespace kv {

// Byte-exact comparison between a stored std::string and an incoming
// string_view.  Used by every engine's lookup path.
inline bool equals(const std::string& stored, std::string_view key) noexcept {
    return stored.size() == key.size() &&
           (key.empty() || std::memcmp(stored.data(), key.data(), key.size()) == 0);
}

// Callback type for Engine::for_each.  Both views are valid only for the
// duration of the call.
using VisitFn = std::function<void(std::string_view key, std::string_view value)>;

// Every storage engine implements the same six operations, which is the
// "5+2 interface" of the original project expressed as a C++ abstract class.
//
// Thread-safety: engines are deliberately *not* synchronised.  Concurrency is
// provided one layer up by ShardedStore, which hands out at most one thread
// per engine instance under an exclusive lock.  That keeps each engine simple
// and makes the locking policy testable in one place.
class Engine {
public:
    virtual ~Engine() = default;

    // Identifier used on the command line, e.g. "rbtree".
    virtual const char* name() const noexcept = 0;

    // Insert or overwrite.  Always Ok unless the key is empty.
    virtual Status put(std::string_view key, std::string_view value) = 0;

    // Copy of the stored value, or nullopt when the key is absent.
    virtual std::optional<std::string> get(std::string_view key) const = 0;

    // Remove a key.  NotFound when it was not there.
    virtual Status erase(std::string_view key) = 0;

    // Update in place; unlike put() this never creates a missing key.
    virtual Status update(std::string_view key, std::string_view value) = 0;

    virtual std::size_t size() const noexcept = 0;

    virtual void clear() = 0;

    // Visits every live key/value pair exactly once, in whatever order the
    // structure happens to produce (sorted for the red-black tree, arbitrary
    // for the others).  Used by the KEYS command and by diagnostics.
    //
    // The callback must not mutate the engine.  Callers hold a shared lock for
    // the whole scan, so this is safe against concurrent writers but it is
    // still an O(n) operation that blocks writes to that shard.
    virtual void for_each(const VisitFn& visit) const = 0;
};

// Factory used by the server and by main() to pick an engine by name.
// Returns nullptr for an unknown name.
std::unique_ptr<Engine> make_engine(std::string_view name);

// "array, hash, unordered, rbtree, skiplist"
std::string engine_names();

}  // namespace kv
