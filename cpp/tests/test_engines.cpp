#include "test_framework.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "kv/engines.hpp"

namespace {

using kv::ArrayEngine;
using kv::Engine;
using kv::HashEngine;
using kv::RBTreeEngine;
using kv::SkipListEngine;
using kv::Status;
using kv::UnorderedEngine;

std::string key_of(int i) { return "key:" + std::to_string(i); }

// Every engine must satisfy the same contract; the whole suite below is run
// against all five implementations so none of them can drift.
void conformance(Engine& engine, const char* label) {
    engine.clear();
    KV_CHECK_EQ(engine.size(), 0);
    KV_CHECK(!engine.get("absent").has_value());
    KV_CHECK_STR(engine.name(), label);

    // set / get
    KV_CHECK(engine.put("alpha", "1") == Status::Ok);
    KV_CHECK(engine.put("beta", "2") == Status::Ok);
    KV_CHECK_EQ(engine.size(), 2);
    KV_CHECK_STR(*engine.get("alpha"), "1");
    KV_CHECK_STR(*engine.get("beta"), "2");

    // overwrite does not duplicate
    KV_CHECK(engine.put("alpha", "one") == Status::Ok);
    KV_CHECK_EQ(engine.size(), 2);
    KV_CHECK_STR(*engine.get("alpha"), "one");

    // update only touches existing keys
    KV_CHECK(engine.update("alpha", "uno") == Status::Ok);
    KV_CHECK_STR(*engine.get("alpha"), "uno");
    KV_CHECK(engine.update("gamma", "3") == Status::NotFound);
    KV_CHECK(!engine.get("gamma").has_value());

    // for_each must visit every live pair exactly once, and only live ones.
    // Ordering is engine-defined, so only membership is asserted here.
    {
        std::size_t visited = 0;
        std::string seen;
        bool beta_value_ok = false;
        engine.for_each([&](std::string_view key, std::string_view value) {
            ++visited;
            seen.append(key);
            seen.push_back(';');
            if (key == "beta") {
                beta_value_ok = (value == "2");
            }
        });
        KV_CHECK_EQ(visited, 2);
        KV_CHECK(seen.find("alpha;") != std::string::npos);
        KV_CHECK(seen.find("beta;") != std::string::npos);
        KV_CHECK(beta_value_ok);
    }

    // erase
    KV_CHECK(engine.erase("beta") == Status::Ok);
    KV_CHECK(engine.erase("beta") == Status::NotFound);
    KV_CHECK_EQ(engine.size(), 1);
    KV_CHECK(!engine.get("beta").has_value());
    KV_CHECK_STR(*engine.get("alpha"), "uno");

    // empty key is rejected consistently
    KV_CHECK(engine.put("", "x") == Status::InvalidArgument);

    // empty value is legal
    KV_CHECK(engine.put("empty", "") == Status::Ok);
    KV_CHECK(engine.get("empty").has_value());
    KV_CHECK_STR(*engine.get("empty"), "");

    // binary-ish payloads with spaces and CRLF survive untouched
    const std::string tricky = "a b\tc\r\nd";
    KV_CHECK(engine.put("tricky", tricky) == Status::Ok);
    KV_CHECK_STR(*engine.get("tricky"), tricky);

    engine.clear();
    KV_CHECK_EQ(engine.size(), 0);
    KV_CHECK(!engine.get("alpha").has_value());
}

void bulk_roundtrip(Engine& engine) {
    constexpr int kCount = 2000;
    engine.clear();
    for (int i = 0; i < kCount; ++i) {
        KV_CHECK(engine.put(key_of(i), std::to_string(i)) == Status::Ok);
    }
    KV_CHECK_EQ(engine.size(), kCount);
    for (int i = 0; i < kCount; ++i) {
        auto value = engine.get(key_of(i));
        KV_CHECK(value.has_value());
        if (value) {
            KV_CHECK_STR(*value, std::to_string(i));
        }
    }
    // remove every other key, then confirm the survivors and the count
    for (int i = 0; i < kCount; i += 2) {
        KV_CHECK(engine.erase(key_of(i)) == Status::Ok);
    }
    KV_CHECK_EQ(engine.size(), kCount / 2);
    for (int i = 0; i < kCount; ++i) {
        const bool should_exist = (i % 2) != 0;
        KV_CHECK(engine.get(key_of(i)).has_value() == should_exist);
    }
}

}  // namespace

KV_TEST(engine_array_conformance) {
    ArrayEngine engine;
    conformance(engine, "array");
}

KV_TEST(engine_hash_conformance) {
    HashEngine engine;
    conformance(engine, "hash");
}

KV_TEST(engine_unordered_conformance) {
    UnorderedEngine engine;
    conformance(engine, "unordered");
}

KV_TEST(engine_rbtree_conformance) {
    RBTreeEngine engine;
    conformance(engine, "rbtree");
}

KV_TEST(engine_skiplist_conformance) {
    SkipListEngine engine;
    conformance(engine, "skiplist");
}

KV_TEST(engine_hash_bulk) {
    HashEngine engine;
    bulk_roundtrip(engine);
}

KV_TEST(engine_rbtree_bulk) {
    RBTreeEngine engine;
    bulk_roundtrip(engine);
    KV_CHECK(engine.check_invariants());
}

KV_TEST(engine_skiplist_bulk) {
    SkipListEngine engine;
    bulk_roundtrip(engine);
    KV_CHECK(engine.check_invariants());
}

KV_TEST(engine_unordered_bulk) {
    UnorderedEngine engine;
    bulk_roundtrip(engine);
}

// Regression for legacy/kvstore_hash.c: deletion walked the chain but compared
// the *head* key every step, so only the first node of a bucket could ever be
// removed.  Here every key is deleted in reverse insertion order, which
// guarantees the head is rarely the target.
KV_TEST(engine_hash_deletes_non_head_nodes) {
    HashEngine engine;
    // These keys share a bucket because the engine is small by construction.
    std::vector<std::string> chain;
    for (int i = 0; i < 64; ++i) {
        chain.push_back("collide:" + std::to_string(i));
        KV_CHECK(engine.put(chain.back(), "v") == Status::Ok);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        KV_CHECK(engine.erase(*it) == Status::Ok);
    }
    KV_CHECK_EQ(engine.size(), 0);
}

// The C hash used sum-of-bytes, so every anagram landed in the same bucket.
KV_TEST(engine_hash_separates_anagrams) {
    HashEngine engine;
    const std::vector<std::string> anagrams = {"abc", "acb", "bac", "bca", "cab", "cba"};
    for (const auto& key : anagrams) {
        KV_CHECK(engine.put(key, key) == Status::Ok);
    }
    KV_CHECK_EQ(engine.size(), anagrams.size());
    for (const auto& key : anagrams) {
        KV_CHECK_STR(*engine.get(key), key);
    }
}

KV_TEST(engine_hash_grows) {
    HashEngine engine(16);
    const std::size_t initial = engine.bucket_count();
    for (int i = 0; i < 1000; ++i) {
        engine.put(key_of(i), "v");
    }
    KV_CHECK(engine.bucket_count() > initial);
    KV_CHECK(engine.load_factor() <= 1.0);
    for (int i = 0; i < 1000; ++i) {
        KV_CHECK(engine.get(key_of(i)).has_value());
    }
}

// Regression for legacy/kvstore_array.c: erase() decremented the index without
// moving the tail element, so one unrelated record vanished per deletion.
KV_TEST(engine_array_erase_keeps_tail) {
    ArrayEngine engine;
    for (int i = 0; i < 10; ++i) {
        engine.put(key_of(i), std::to_string(i));
    }
    KV_CHECK(engine.erase(key_of(0)) == Status::Ok);
    KV_CHECK_EQ(engine.size(), 9);
    for (int i = 1; i < 10; ++i) {
        KV_CHECK(engine.get(key_of(i)).has_value());
    }
}

// Randomised differential test: the red-black tree must agree with std::map
// after every operation, and must still satisfy the five RB invariants.
KV_TEST(engine_rbtree_matches_std_map) {
    RBTreeEngine engine;
    std::map<std::string, std::string> reference;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> key_dist(0, 500);
    std::uniform_int_distribution<int> op_dist(0, 9);

    for (int step = 0; step < 20000; ++step) {
        const std::string key = key_of(key_dist(rng));
        const int op = op_dist(rng);

        if (op < 5) {
            const std::string value = "v" + std::to_string(step);
            engine.put(key, value);
            reference[key] = value;
        } else if (op < 7) {
            const bool erased = engine.erase(key) == Status::Ok;
            const bool expected = reference.erase(key) > 0;
            KV_CHECK(erased == expected);
        } else if (op < 9) {
            const std::string value = "m" + std::to_string(step);
            const bool updated = engine.update(key, value) == Status::Ok;
            auto it = reference.find(key);
            const bool expected = it != reference.end();
            KV_CHECK(updated == expected);
            if (expected) {
                it->second = value;
            }
        } else {
            const auto value = engine.get(key);
            auto it = reference.find(key);
            KV_CHECK(value.has_value() == (it != reference.end()));
            if (value && it != reference.end()) {
                KV_CHECK_STR(*value, it->second);
            }
        }

        if (step % 500 == 0) {
            KV_CHECK(engine.check_invariants());
            KV_CHECK_EQ(engine.size(), reference.size());
        }
    }

    KV_CHECK(engine.check_invariants());
    KV_CHECK_EQ(engine.size(), reference.size());
    KV_CHECK(reference.size() > 100);  // the test actually exercised a big tree
    // A red-black tree with n nodes has height <= 2*log2(n+1).
    const double bound = 2.0 * std::log2(static_cast<double>(engine.size()) + 1.0);
    KV_CHECK(static_cast<double>(engine.height()) <= bound + 1.0);
}

KV_TEST(engine_skiplist_matches_std_map) {
    SkipListEngine engine;
    std::map<std::string, std::string> reference;
    std::mt19937 rng(999);
    std::uniform_int_distribution<int> key_dist(0, 400);
    std::uniform_int_distribution<int> op_dist(0, 9);

    for (int step = 0; step < 10000; ++step) {
        const std::string key = key_of(key_dist(rng));
        const int op = op_dist(rng);
        if (op < 5) {
            const std::string value = "v" + std::to_string(step);
            engine.put(key, value);
            reference[key] = value;
        } else if (op < 7) {
            const bool erased = engine.erase(key) == Status::Ok;
            KV_CHECK(erased == (reference.erase(key) > 0));
        } else if (op < 9) {
            const std::string value = "m" + std::to_string(step);
            const bool updated = engine.update(key, value) == Status::Ok;
            auto it = reference.find(key);
            KV_CHECK(updated == (it != reference.end()));
            if (it != reference.end()) {
                it->second = value;
            }
        } else {
            const auto value = engine.get(key);
            auto it = reference.find(key);
            KV_CHECK(value.has_value() == (it != reference.end()));
            if (value && it != reference.end()) {
                KV_CHECK_STR(*value, it->second);
            }
        }
        if (step % 500 == 0) {
            KV_CHECK(engine.check_invariants());
            KV_CHECK_EQ(engine.size(), reference.size());
        }
    }
    KV_CHECK(engine.check_invariants());
    KV_CHECK(engine.level() <= SkipListEngine::kMaxLevel);
}

// Deleting every key must leave an empty structure that still works.
KV_TEST(engine_rbtree_empty_after_full_erase) {    RBTreeEngine engine;
    for (int i = 0; i < 100; ++i) {
        engine.put(key_of(i), "v");
    }
    for (int i = 0; i < 100; ++i) {
        KV_CHECK(engine.erase(key_of(i)) == Status::Ok);
    }
    KV_CHECK_EQ(engine.size(), 0);
    KV_CHECK(engine.check_invariants());
    KV_CHECK_EQ(engine.height(), 0);
    // and it is still usable afterwards
    KV_CHECK(engine.put("again", "1") == Status::Ok);
    KV_CHECK(engine.check_invariants());
    KV_CHECK_STR(*engine.get("again"), "1");
}

// The red-black tree's in-order traversal is what makes KEYS output sorted for
// that engine; the other engines make no ordering promise.
KV_TEST(engine_rbtree_for_each_is_sorted) {
    RBTreeEngine engine;
    for (const auto* key : {"delta", "alpha", "charlie", "bravo"}) {
        engine.put(key, "v");
    }
    std::vector<std::string> seen;
    engine.for_each([&seen](std::string_view key, std::string_view) {
        seen.emplace_back(key);
    });
    KV_CHECK_EQ(seen.size(), 4);
    KV_CHECK(std::is_sorted(seen.begin(), seen.end()));
}

KV_TEST(engine_skiplist_for_each_is_sorted) {
    SkipListEngine engine;
    for (const auto* key : {"delta", "alpha", "charlie", "bravo"}) {
        engine.put(key, "v");
    }
    std::vector<std::string> seen;
    engine.for_each([&seen](std::string_view key, std::string_view) {
        seen.emplace_back(key);
    });
    KV_CHECK_EQ(seen.size(), 4);
    KV_CHECK(std::is_sorted(seen.begin(), seen.end()));
}

// for_each must not report erased keys, and must yield nothing after clear().
KV_TEST(engine_for_each_tracks_erasure) {
    auto check = [](Engine& engine) {
        engine.put("keep", "1");
        engine.put("drop", "2");
        engine.erase("drop");

        std::size_t visited = 0;
        bool saw_dropped = false;
        engine.for_each([&](std::string_view key, std::string_view) {
            ++visited;
            if (key == "drop") {
                saw_dropped = true;
            }
        });
        KV_CHECK_EQ(visited, 1);
        KV_CHECK(!saw_dropped);

        engine.clear();
        std::size_t after_clear = 0;
        engine.for_each([&after_clear](std::string_view, std::string_view) { ++after_clear; });
        KV_CHECK_EQ(after_clear, 0);
    };

    ArrayEngine array;
    check(array);
    HashEngine hash;
    check(hash);
    UnorderedEngine unordered_engine;
    check(unordered_engine);
    RBTreeEngine rbtree;
    check(rbtree);
    SkipListEngine skiplist;
    check(skiplist);
}

