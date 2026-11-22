#include "test_framework.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kv/wal.hpp"

namespace {

using kv::Wal;

struct Record {
    Wal::Op op;
    std::string key;
    std::string value;
};

std::string temp_path(const char* name) { return std::string("kvtests_") + name; }
void remove_file(const std::string& path) { std::remove(path.c_str()); }

std::vector<Record> replay_all(Wal& wal, std::string& note) {
    std::vector<Record> records;
    wal.replay(
        [&records](Wal::Op op, std::string_view key, std::string_view value) {
            records.push_back(Record{op, std::string(key), std::string(value)});
        },
        note);
    return records;
}

std::size_t file_size(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::size_t>(size);
}

}  // namespace

KV_TEST(wal_roundtrip) {
    const std::string path = temp_path("roundtrip.wal");
    remove_file(path);
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            return;
        }
        KV_CHECK(wal->append(Wal::Op::Put, "alpha", "1"));
        KV_CHECK(wal->append(Wal::Op::Put, "beta", "2"));
        KV_CHECK(wal->append(Wal::Op::Erase, "alpha"));
        KV_CHECK(wal->flush());
        KV_CHECK_EQ(wal->records_written(), 3);
        KV_CHECK(wal->bytes_written() > 0);
    }
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            remove_file(path);
            return;
        }
        std::string note;
        const auto records = replay_all(*wal, note);
        KV_CHECK(note.empty());
        KV_CHECK_EQ(records.size(), 3);
        if (records.size() == 3) {
            KV_CHECK(records[0].op == Wal::Op::Put);
            KV_CHECK_STR(records[0].key, "alpha");
            KV_CHECK_STR(records[0].value, "1");
            KV_CHECK(records[2].op == Wal::Op::Erase);
            KV_CHECK_STR(records[2].key, "alpha");
        }
    }
    remove_file(path);
}

// A crash can leave a partially written record.  Recovery must keep everything
// before it and stop cleanly instead of replaying garbage.
KV_TEST(wal_ignores_truncated_tail) {
    const std::string path = temp_path("truncated.wal");
    remove_file(path);
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            return;
        }
        for (int i = 0; i < 10; ++i) {
            wal->append(Wal::Op::Put, "key" + std::to_string(i), "value");
        }
        KV_CHECK(wal->flush());
    }

    const std::size_t full_size = file_size(path);
    KV_CHECK(full_size > 10);
    {
        std::error_code ec;
        std::filesystem::resize_file(path, full_size - 5, ec);  // chop the last record
        KV_CHECK(!ec);
    }

    // Scope the WAL so the file handle is closed before we delete it (on
    // Windows an open handle makes remove() fail).
    std::string error;
    {
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (wal) {
            std::string note;
            const auto records = replay_all(*wal, note);
            KV_CHECK_EQ(records.size(), 9);  // the last one is dropped
            KV_CHECK(!note.empty());         // and the caller is told why
        }
    }
    remove_file(path);
}

// A bit flip in the payload must be detected by the per-record CRC.
KV_TEST(wal_detects_corruption) {
    const std::string path = temp_path("corrupt.wal");
    remove_file(path);
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            return;
        }
        for (int i = 0; i < 5; ++i) {
            KV_CHECK(wal->append(Wal::Op::Put, "key" + std::to_string(i), "0123456789"));
        }
        KV_CHECK(wal->flush());
    }

    // Flip a byte inside the payload of the third record.
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        KV_CHECK(file.good());
        file.seekp(static_cast<std::streamoff>(file_size(path) / 2));
        const char junk = '\x7f';
        file.write(&junk, 1);
        file.flush();
    }

    std::string error;
    {
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (wal) {
            std::string note;
            const auto records = replay_all(*wal, note);
            KV_CHECK(records.size() < 5);
            KV_CHECK(!note.empty());
        }
    }
    remove_file(path);
}

KV_TEST(wal_crc32_known_vector) {
    // CRC-32/IEEE of "123456789" is 0xCBF43926.
    const char* input = "123456789";
    KV_CHECK_EQ(kv::crc32(input, 9), 0xCBF43926u);
}

KV_TEST(wal_survives_large_values) {
    const std::string path = temp_path("large.wal");
    remove_file(path);
    const std::string big(64 * 1024, 'z');
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (!wal) {
            return;
        }
        KV_CHECK(wal->append(Wal::Op::Put, "big", big));
        KV_CHECK(wal->flush());
    }
    {
        std::string error;
        auto wal = Wal::open(path, false, error);
        KV_CHECK(wal != nullptr);
        if (wal) {
            std::string note;
            const auto records = replay_all(*wal, note);
            KV_CHECK_EQ(records.size(), 1);
            if (!records.empty()) {
                KV_CHECK_EQ(records[0].value.size(), big.size());
                KV_CHECK(records[0].value == big);
            }
        }
    }
    remove_file(path);
}
