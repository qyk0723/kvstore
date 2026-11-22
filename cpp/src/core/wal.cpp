#include "kv/wal.hpp"

#include <array>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kv {

namespace {

constexpr std::uint32_t kMaxRecord = 1u << 20;  // 1 MiB guard against garbage
constexpr std::size_t kHeaderSize = 8;          // len + crc

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
            }
            t[i] = crc;
        }
        return t;
    }();
    return table;
}

void store_u32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFF);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

std::uint32_t load_u32(const std::uint8_t* in) noexcept {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

void append_u32(std::string& out, std::uint32_t value) {
    char bytes[4];
    store_u32(reinterpret_cast<std::uint8_t*>(bytes), value);
    out.append(bytes, sizeof(bytes));
}

void append_bytes(std::string& out, std::string_view bytes) {
    if (!bytes.empty()) {
        out.append(bytes.data(), bytes.size());
    }
}

int file_no(std::FILE* file) noexcept {
#ifdef _WIN32
    return _fileno(file);
#else
    return ::fileno(file);
#endif
}

bool sync_fd(int fd) noexcept {
#ifdef _WIN32
    return _commit(fd) == 0;
#else
    return ::fsync(fd) == 0;
#endif
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t len) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto& table = crc_table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

Wal::Wal(std::FILE* fp, std::string path, bool fsync_each_write)
    : fp_(fp), path_(std::move(path)), fsync_each_write_(fsync_each_write) {}

Wal::~Wal() {
    if (fp_ != nullptr) {
        std::fflush(fp_);
        if (fsync_each_write_) {
            const int fd = file_no(fp_);
            if (fd >= 0) {
                sync_fd(fd);
            }
        }
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

std::unique_ptr<Wal> Wal::open(const std::string& path, bool fsync_each_write, std::string& err) {
    std::FILE* fp = std::fopen(path.c_str(), "ab");
    if (fp == nullptr) {
        err = "cannot open WAL for append: " + path;
        return nullptr;
    }
    std::unique_ptr<Wal> wal(new Wal(fp, path, fsync_each_write));
    return wal;
}

bool Wal::append(Op op, std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (fp_ == nullptr) {
        return false;
    }

    std::string payload;
    payload.reserve(1 + 4 + key.size() + 4 + value.size());
    payload.push_back(static_cast<char>(op));
    append_u32(payload, static_cast<std::uint32_t>(key.size()));
    append_bytes(payload, key);
    append_u32(payload, static_cast<std::uint32_t>(value.size()));
    append_bytes(payload, value);

    std::string frame;
    frame.resize(kHeaderSize);
    store_u32(reinterpret_cast<std::uint8_t*>(&frame[0]), static_cast<std::uint32_t>(payload.size()));
    store_u32(reinterpret_cast<std::uint8_t*>(&frame[4]), crc32(payload.data(), payload.size()));
    frame.append(payload);

    if (std::fwrite(frame.data(), 1, frame.size(), fp_) != frame.size()) {
        return false;
    }
    // Always hand the bytes to the kernel, so a *process* crash (kill -9)
    // cannot lose acknowledged writes.  fsync additionally protects against a
    // machine/power failure, at the cost of one disk flush per write.
    if (std::fflush(fp_) != 0) {
        return false;
    }
    if (fsync_each_write_) {
        const int fd = file_no(fp_);
        if (fd < 0 || !sync_fd(fd)) {
            return false;
        }
    }

    bytes_.fetch_add(frame.size(), std::memory_order_relaxed);
    records_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t Wal::replay(
    const std::function<void(Op, std::string_view, std::string_view)>& fn, std::string& err) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (fp_ != nullptr) {
        std::fflush(fp_);
    }

    std::FILE* input = std::fopen(path_.c_str(), "rb");
    if (input == nullptr) {
        err = "cannot open WAL for replay: " + path_;
        return 0;
    }

    std::size_t replayed = 0;
    std::vector<std::uint8_t> header(kHeaderSize);
    std::vector<char> payload;
    std::string key;
    std::string value;

    for (;;) {
        const std::size_t got = std::fread(header.data(), 1, kHeaderSize, input);
        if (got == 0) {
            break;  // clean end of log
        }
        if (got != kHeaderSize) {
            err = "truncated record header at end of WAL (ignored)";
            break;
        }

        const std::uint32_t len = load_u32(header.data());
        const std::uint32_t expected_crc = load_u32(header.data() + 4);
        if (len == 0 || len > kMaxRecord) {
            err = "corrupt record length at offset " + std::to_string(replayed);
            break;
        }

        payload.resize(len);
        const std::size_t body = std::fread(payload.data(), 1, len, input);
        if (body != len) {
            err = "truncated record body at end of WAL (ignored)";
            break;
        }
        if (crc32(payload.data(), len) != expected_crc) {
            err = "record checksum mismatch, stopping replay";
            break;
        }

        std::size_t offset = 0;
        const auto op = static_cast<Op>(static_cast<std::uint8_t>(payload[offset]));
        offset += 1;
        if (offset + 4 > len) {
            err = "malformed record";
            break;
        }
        const std::uint32_t klen = load_u32(reinterpret_cast<const std::uint8_t*>(payload.data() + offset));
        offset += 4;
        if (offset + klen > len) {
            err = "malformed record";
            break;
        }
        key.assign(payload.data() + offset, klen);
        offset += klen;

        if (offset + 4 > len) {
            err = "malformed record";
            break;
        }
        const std::uint32_t vlen = load_u32(reinterpret_cast<const std::uint8_t*>(payload.data() + offset));
        offset += 4;
        if (offset + vlen > len) {
            err = "malformed record";
            break;
        }
        value.assign(payload.data() + offset, vlen);
        offset += vlen;

        fn(op, key, value);
        ++replayed;
    }

    std::fclose(input);
    return replayed;
}

bool Wal::flush() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (fp_ == nullptr) {
        return false;
    }
    return std::fflush(fp_) == 0;
}

bool Wal::sync() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (fp_ == nullptr) {
        return false;
    }
    if (std::fflush(fp_) != 0) {
        return false;
    }
    const int fd = file_no(fp_);
    return fd >= 0 && sync_fd(fd);
}

}  // namespace kv
