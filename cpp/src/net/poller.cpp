#include "kv/net/poller.hpp"

#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#else
#ifndef _WIN32
#include <poll.h>
#endif
#endif

namespace kv::net {

// ===========================================================================
// Linux backend: epoll (level-triggered).
//
// Level-triggered is chosen deliberately: the handlers already drain every
// readable socket until EAGAIN, so edge-triggered would buy nothing but would
// require every future user of the interface to remember the "read until
// EAGAIN" rule or hang forever.
// ===========================================================================
#if defined(__linux__)

namespace {

std::uint32_t to_epoll(std::uint32_t events) noexcept {
    std::uint32_t out = EPOLLRDHUP;
    if (events & kEventRead) {
        out |= EPOLLIN;
    }
    if (events & kEventWrite) {
        out |= EPOLLOUT;
    }
    return out;
}

class EpollPoller final : public Poller {
public:
    EpollPoller() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {}
    ~EpollPoller() override {
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
        }
    }

    bool valid() const noexcept { return epoll_fd_ >= 0; }

    bool add(socket_t fd, std::uint32_t events, std::string& err) override {
        epoll_event event{};
        event.events = to_epoll(events);
        event.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
            err = "epoll_ctl(ADD) failed";
            return false;
        }
        return true;
    }

    bool mod(socket_t fd, std::uint32_t events, std::string& err) override {
        epoll_event event{};
        event.events = to_epoll(events);
        event.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) != 0) {
            err = "epoll_ctl(MOD) failed";
            return false;
        }
        return true;
    }

    bool del(socket_t fd) override {
        return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    int wait(ReadyEvent* out, int max_events, int timeout_ms) override {
        int count = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()),
                                 timeout_ms);
        if (count < 0) {
            return 0;  // EINTR and friends: just let the caller loop again
        }
        for (int i = 0; i < count; ++i) {
            const epoll_event& raw = events_[static_cast<std::size_t>(i)];
            ReadyEvent& ready = out[i];
            ready.fd = raw.data.fd;
            ready.events = kEventNone;
            if (raw.events & (EPOLLIN | EPOLLPRI)) {
                ready.events |= kEventRead;
            }
            if (raw.events & EPOLLOUT) {
                ready.events |= kEventWrite;
            }
            ready.peer_closed = (raw.events & (EPOLLRDHUP | EPOLLHUP)) != 0;
            ready.failed = (raw.events & EPOLLERR) != 0;
        }
        return count;
    }

private:
    int epoll_fd_ = -1;
    std::vector<epoll_event> events_ = std::vector<epoll_event>(256);
};

}  // namespace

std::unique_ptr<Poller> Poller::create() {
    auto poller = std::make_unique<EpollPoller>();
    if (!poller->valid()) {
        return nullptr;
    }
    return poller;
}

const char* Poller::backend_name() noexcept { return "epoll"; }

// ===========================================================================
// Portable backend: WSAPoll on Windows, poll() elsewhere.
//
// This exists so the reactor, the protocol and the store can be built and
// tested on a machine without a Linux kernel.  It is not the deployment
// target; the interface is identical, so nothing above this line changes.
// ===========================================================================
#else

namespace {

#ifdef _WIN32
using pollfd_type = WSAPOLLFD;
int poll_wait(pollfd_type* fds, unsigned long count, int timeout) {
    return ::WSAPoll(fds, count, timeout);
}
// WSAPoll documents POLLRDNORM/POLLWRNORM as the request bits for sockets; the
// POSIX-style POLLIN also carries POLLRDBAND, which sockets here do not use.
constexpr short kPollRead = POLLRDNORM;
constexpr short kPollWrite = POLLWRNORM;
#else
using pollfd_type = struct pollfd;
int poll_wait(pollfd_type* fds, unsigned long count, int timeout) {
    return ::poll(fds, static_cast<nfds_t>(count), timeout);
}
constexpr short kPollRead = POLLIN;
constexpr short kPollWrite = POLLOUT;
#endif

short to_poll(std::uint32_t events) noexcept {
    short out = kPollRead;
    if (events & kEventWrite) {
        out = static_cast<short>(out | kPollWrite);
    }
    return out;
}

class PollPoller final : public Poller {
public:
    bool add(socket_t fd, std::uint32_t events, std::string& err) override {
        if (index_.count(fd) != 0) {
            err = "poller: fd already registered";
            return false;
        }
        pollfd_type entry{};
        entry.fd = fd;
        entry.events = to_poll(events);
        entry.revents = 0;
        index_[fd] = fds_.size();
        fds_.push_back(entry);
        return true;
    }

    bool mod(socket_t fd, std::uint32_t events, std::string& err) override {
        auto it = index_.find(fd);
        if (it == index_.end()) {
            err = "poller: fd not registered";
            return false;
        }
        fds_[it->second].events = to_poll(events);
        return true;
    }

    bool del(socket_t fd) override {
        auto it = index_.find(fd);
        if (it == index_.end()) {
            return false;
        }
        const std::size_t slot = it->second;
        // Swap-remove keeps deletion O(1); the moved entry needs its index
        // fixed up, which is why index_ exists at all.
        if (slot + 1 != fds_.size()) {
            fds_[slot] = fds_.back();
            index_[fds_[slot].fd] = slot;
        }
        fds_.pop_back();
        index_.erase(it);
        return true;
    }

    int wait(ReadyEvent* out, int max_events, int timeout_ms) override {
        for (auto& entry : fds_) {
            entry.revents = 0;
        }
        const int count = poll_wait(fds_.data(), static_cast<unsigned long>(fds_.size()), timeout_ms);
        if (count <= 0) {
            return 0;
        }
        int written = 0;
        for (const auto& entry : fds_) {
            if (entry.revents == 0) {
                continue;
            }
            if (written >= max_events) {
                break;
            }
            ReadyEvent& ready = out[written++];
            ready.fd = entry.fd;
            ready.events = kEventNone;
            if ((entry.revents & kPollRead) != 0) {
                ready.events |= kEventRead;
            }
            if ((entry.revents & kPollWrite) != 0) {
                ready.events |= kEventWrite;
            }
            ready.peer_closed = (entry.revents & POLLHUP) != 0;
            ready.failed = (entry.revents & POLLERR) != 0;
        }
        return written;
    }

private:
    std::vector<pollfd_type> fds_;
    std::unordered_map<socket_t, std::size_t> index_;
};

}  // namespace

std::unique_ptr<Poller> Poller::create() { return std::make_unique<PollPoller>(); }

const char* Poller::backend_name() noexcept { return "poll"; }

#endif

}  // namespace kv::net
