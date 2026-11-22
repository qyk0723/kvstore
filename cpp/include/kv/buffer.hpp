#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace kv {

// Growable byte queue used for both the per-connection read buffer and the
// pending-write buffer.
//
// The original project read straight into a fixed char[128] and never used the
// recorded length, which is why it could not reassemble a request that arrived
// in two TCP segments.
//
// Implementation note: the storage vector is kept at its *capacity* size and
// two cursors (read/write) track the live window.  The obvious alternative --
// vector::resize() after memcpy -- silently zero-fills the freshly "added"
// elements and wipes the data, which is exactly the bug this class had in its
// first revision.
class Buffer {
public:
    explicit Buffer(std::size_t initial = 1024) {
        if (initial == 0) {
            initial = 256;
        }
        data_.resize(initial);
    }

    void append(std::string_view bytes);

    // Bytes that have been received but not consumed yet.
    std::string_view readable() const noexcept {
        return std::string_view(data_.data() + read_pos_, write_pos_ - read_pos_);
    }

    std::size_t size() const noexcept { return write_pos_ - read_pos_; }
    bool empty() const noexcept { return read_pos_ == write_pos_; }

    void consume(std::size_t n) noexcept {
        read_pos_ += n;
        if (read_pos_ >= write_pos_) {
            clear();
        }
    }

    void clear() noexcept {
        read_pos_ = 0;
        write_pos_ = 0;
    }

    // --- zero-copy read support -------------------------------------------
    // Call ensure_writable(n) first and re-acquire begin_write() afterwards,
    // because it may reallocate.
    char* begin_write() noexcept { return data_.data() + write_pos_; }
    std::size_t writable() const noexcept { return data_.size() - write_pos_; }

    void ensure_writable(std::size_t n);
    void commit(std::size_t n) noexcept { write_pos_ += n; }

    std::size_t capacity() const noexcept { return data_.size(); }

private:
    void compact();

    std::vector<char> data_;  // always data_.size() == capacity
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
};

}  // namespace kv
