// fd_emul.cpp -- the eventfd and epoll libutils' Looper is written on, over Win32 events.
//
// HWUI's render thread runs a Looper (ThreadBase) that sleeps in epoll_wait on its wake eventfd and
// on the choreographer's descriptor (AChoreographer_getFd). Windows has neither, so both are
// emulated descriptors: numbers from kBase up, each a counter and a manual-reset event that is set
// while the counter is not zero (level-triggered, as eventfd under epoll is). An epoll instance is
// a set of them, waited on with WaitForMultipleObjects. Any other descriptor given to read, write
// or close in a translation unit that includes sys/eventfd.h goes to the CRT.
#include <sys/epoll.h>
#include <sys/eventfd.h>
#undef read
#undef write
#undef close

#include <windows.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "fd_emul.h"

namespace {

constexpr int kBase = 1 << 24;

struct EventFd {
    HANDLE event = nullptr;  // set while count != 0
    uint64_t count = 0;
    bool nonblock = false;
};

struct Epoll {
    std::map<int, epoll_event> watched;
};

std::mutex g_lock;
std::map<int, std::shared_ptr<EventFd>> g_events;
std::map<int, std::shared_ptr<Epoll>> g_polls;
int g_next = kBase;

std::shared_ptr<EventFd> event_of(int fd) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_events.find(fd);
    return it == g_events.end() ? nullptr : it->second;
}

}  // namespace

extern "C" int winapk_eventfd(unsigned int initval, int flags) {
    auto e = std::make_shared<EventFd>();
    e->event = CreateEventW(nullptr, TRUE, initval != 0, nullptr);
    if (!e->event) {
        errno = ENOMEM;
        return -1;
    }
    e->count = initval;
    e->nonblock = (flags & EFD_NONBLOCK) != 0;
    std::lock_guard<std::mutex> hold(g_lock);
    const int fd = g_next++;
    g_events[fd] = e;
    return fd;
}

extern "C" long long winapk_fd_write(int fd, const void* buf, size_t n) {
    if (fd < kBase) return _write(fd, buf, static_cast<unsigned>(n));
    auto e = event_of(fd);
    if (!e || n < sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t add;
    std::memcpy(&add, buf, sizeof add);
    std::lock_guard<std::mutex> hold(g_lock);
    e->count += add;
    if (e->count) SetEvent(e->event);
    return sizeof(uint64_t);
}

extern "C" long long winapk_fd_read(int fd, void* buf, size_t n) {
    if (fd < kBase) return _read(fd, buf, static_cast<unsigned>(n));
    auto e = event_of(fd);
    if (!e || n < sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (e->count) {
                const uint64_t c = e->count;
                std::memcpy(buf, &c, sizeof c);
                e->count = 0;
                ResetEvent(e->event);
                return sizeof(uint64_t);
            }
        }
        if (e->nonblock) {
            errno = EAGAIN;
            return -1;
        }
        WaitForSingleObject(e->event, INFINITE);
    }
}

extern "C" int winapk_fd_close(int fd) {
    if (fd < kBase) return _close(fd);
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_events.find(fd);
    if (it != g_events.end()) {
        CloseHandle(it->second->event);
        g_events.erase(it);
        return 0;
    }
    if (g_polls.erase(fd)) return 0;
    errno = EBADF;
    return -1;
}

extern "C" int winapk_epoll_create1(int) {
    std::lock_guard<std::mutex> hold(g_lock);
    const int fd = g_next++;
    g_polls[fd] = std::make_shared<Epoll>();
    return fd;
}

extern "C" int winapk_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto p = g_polls.find(epfd);
    if (p == g_polls.end()) {
        errno = EBADF;
        return -1;
    }
    if (op != EPOLL_CTL_DEL && g_events.find(fd) == g_events.end()) {
        errno = EPERM;  // as epoll says of a descriptor it cannot watch (a regular file)
        return -1;
    }
    switch (op) {
        case EPOLL_CTL_ADD:
            if (p->second->watched.count(fd)) {
                errno = EEXIST;
                return -1;
            }
            p->second->watched[fd] = *event;
            return 0;
        case EPOLL_CTL_MOD:
            if (!p->second->watched.count(fd)) {
                errno = ENOENT;
                return -1;
            }
            p->second->watched[fd] = *event;
            return 0;
        case EPOLL_CTL_DEL:
            if (!p->second->watched.erase(fd)) {
                errno = ENOENT;
                return -1;
            }
            return 0;
    }
    errno = EINVAL;
    return -1;
}

extern "C" int winapk_epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    std::vector<HANDLE> handles;
    std::vector<std::pair<int, epoll_event>> who;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto p = g_polls.find(epfd);
        if (p == g_polls.end()) {
            errno = EBADF;
            return -1;
        }
        for (const auto& [fd, ev] : p->second->watched) {
            auto e = g_events.find(fd);
            if (e == g_events.end()) continue;
            handles.push_back(e->second->event);
            who.emplace_back(fd, ev);
            if (handles.size() == MAXIMUM_WAIT_OBJECTS) break;
        }
    }
    const DWORD ms = timeout < 0 ? INFINITE : static_cast<DWORD>(timeout);
    if (handles.empty()) {
        Sleep(ms == INFINITE ? 0 : ms);
        return 0;
    }
    const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, ms);
    if (r == WAIT_TIMEOUT) return 0;
    if (r >= WAIT_OBJECT_0 + handles.size()) {
        errno = EINTR;
        return -1;
    }
    // Every descriptor that is ready, not only the first.
    int n = 0;
    for (size_t i = 0; i < handles.size() && n < maxevents; i++) {
        if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) {
            events[n].events = EPOLLIN & who[i].second.events;
            events[n].data = who[i].second.data;
            n++;
        }
    }
    return n;
}

namespace winapk::hwui {

int emulated_eventfd() { return winapk_eventfd(0, EFD_NONBLOCK); }

void emulated_signal(int fd) {
    const uint64_t one = 1;
    winapk_fd_write(fd, &one, sizeof one);
}

bool emulated_drain(int fd) {
    uint64_t c = 0;
    return winapk_fd_read(fd, &c, sizeof c) == sizeof c && c != 0;
}

}  // namespace winapk::hwui
