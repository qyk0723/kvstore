#include "test_framework.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "kv/engines.hpp"
#include "kv/store.hpp"
#include "kv/wal.hpp"

namespace {

using kv::ShardedStore;
using kv::Status;
using kv::Wal;

std::string key_of(int i) { return "key:" + std::to_string(i); }

std::string temp_path(const char* name) { return std::string("kvtests_") + name; }

void remove_file(const std::string& path) { std::remove(path.c_str()); }

}  // namespace

KV_TEST(store_basic_operations) {
    ShardedStore store("hash", 4);
    KV_CHECK_EQ(store.size(), 0);
    KV_CHECK(store.put("a", "1") == Status::Ok);
    KV_CHECK(store.put("b", "2") == Status::Ok);
    KV_CHECK_EQ(store.size(), 2);
    KV_CHECK_STR(*store.get("a"), "1");
    KV_CHECK(store.update("a", "11") == Status::Ok);
    KV_CHECK_STR(*store.get("a"), "11");
    KV_CHECK(store.update("missing", "x") == Status::NotFound);
    KV_CHECK(store.erase("b") == Status::Ok);
    KV_CHECK(store.erase("b") == Status::NotFound);
    KV_CHECK_EQ(store.size(), 1);
    store.clear();
    KV_CHECK_EQ(store.size(), 0);
}

KV_TEST(store_distributes_across_shards) {
    constexpr std::size_t kShards = 8;
    ShardedStore store("hash", kShards);
    for (int i = 0; i < 2000; ++i) {
        store.put(key_of(i), std::to_string(i));
    }
    const auto sizes = store.shard_sizes();
    KV_CHECK_EQ(sizes.size(), kShards);
    std::size_t total = 0;
    for (const auto shard_size : sizes) {
        total += shard_size;
        KV_CHECK(shard_size > 0);  // a 2000-key set must touch every shard
    }
    KV_CHECK_EQ(total, 2000);
}

// The store must be usable from many threads at once.  The original project
// had no locking at all, so this is the test that would have caught it.
KV_TEST(store_is_thread_safe) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    ShardedStore store("rbtree", 16);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key = key_of(t * kPerThread + i);
                store.put(key, std::to_string(i));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    KV_CHECK_EQ(store.size(), static_cast<std::size_t>(kThreads * kPerThread));

    // Concurrent readers while a writer updates a disjoint key range.
    std::atomic<int> mismatches{0};
    threads.clear();
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, &mismatches, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key = key_of(t * kPerThread + i);
                const auto value = store.get(key);
                if (!value.has_value() || *value != std::to_string(i)) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    threads.emplace_back([&store] {
        for (int i = 0; i < kPerThread; ++i) {
            store.update("concurrent-writer:" + std::to_string(i), "x");
        }
    });
    for (auto& thread : threads) {
        thread.join();
    }
    KV_CHECK_EQ(mismatches.load(), 0);
}

KV_TEST(store_can_replay_a_wal) {
    const std::string path = temp_path("replay.wal");
    remove_file(path);

    // Write through a store with a WAL attached.
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            return;
        }
        ShardedStore store("hash", 4, wal.get());
        for (int i = 0; i < 500; ++i) {
            store.put(key_of(i), std::to_string(i));
        }
        for (int i = 0; i < 100; ++i) {
            store.erase(key_of(i));
        }
        KV_CHECK(wal->flush());
    }

    // Recover into a fresh store by replaying the log.
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            remove_file(path);
            return;
        }
        ShardedStore recovered("hash", 4, nullptr);
        std::string note;
        const std::size_t replayed = wal->replay(
            [&recovered](Wal::Op op, std::string_view key, std::string_view value) {
                if (op == Wal::Op::Put) {
                    recovered.put(key, value);
                } else {
                    recovered.erase(key);
                }
            },
            note);
        KV_CHECK_EQ(replayed, 600);  // 500 puts + 100 erases
        KV_CHECK_EQ(recovered.size(), 400);
        for (int i = 0; i < 100; ++i) {
            KV_CHECK(!recovered.get(key_of(i)).has_value());
        }
        for (int i = 100; i < 500; ++i) {
            KV_CHECK_STR(*recovered.get(key_of(i)), std::to_string(i));
        }
    }

    remove_file(path);
}
