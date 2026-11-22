#include "kv/buffer.hpp"

#include <cstring>

namespace kv {

void Buffer::append(std::string_view bytes) {
    ensure_writable(bytes.size());
    std::memcpy(data_.data() + write_pos_, bytes.data(), bytes.size());
    write_pos_ += bytes.size();
}

void Buffer::ensure_writable(std::size_t n) {
    if (data_.size() - write_pos_ >= n) {
        return;
    }
    compact();
    if (data_.size() - write_pos_ >= n) {
        return;
    }
    const std::size_t needed = write_pos_ + n;
    const std::size_t doubled = data_.size() * 2;
    // resize() preserves [0, size) and value-initialises the new tail, which is
    // fine here: only [read_pos_, write_pos_) is ever treated as live data.
    data_.resize(doubled > needed ? doubled : needed);
}

void Buffer::compact() {
    if (read_pos_ == 0) {
        return;
    }
    const std::size_t remain = write_pos_ - read_pos_;
    if (remain != 0) {
        std::memmove(data_.data(), data_.data() + read_pos_, remain);
    }
    read_pos_ = 0;
    write_pos_ = remain;
}

}  // namespace kv
