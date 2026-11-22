#include "kv/engines.hpp"

namespace kv {

Status UnorderedEngine::put(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    map_.insert_or_assign(std::string(key), std::string(value));
    return Status::Ok;
}

std::optional<std::string> UnorderedEngine::get(std::string_view key) const {
    auto it = map_.find(std::string(key));
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

Status UnorderedEngine::erase(std::string_view key) {
    auto it = map_.find(std::string(key));
    if (it == map_.end()) {
        return Status::NotFound;
    }
    map_.erase(it);
    return Status::Ok;
}

Status UnorderedEngine::update(std::string_view key, std::string_view value) {
    auto it = map_.find(std::string(key));
    if (it == map_.end()) {
        return Status::NotFound;
    }
    it->second.assign(value);
    return Status::Ok;
}

void UnorderedEngine::for_each(const VisitFn& visit) const {
    for (const auto& entry : map_) {
        visit(entry.first, entry.second);
    }
}

}  // namespace kv
