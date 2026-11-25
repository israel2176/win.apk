// sys/epoll.h for libutils' Looper on Windows: epoll over hwui_glue's emulated descriptors
// (fd_emul.cpp). Only those descriptors can be watched: the Looper's wake eventfd and HWUI's
// choreographer, the only ones the render thread's Looper is given.
#pragma once
#include <stdint.h>

typedef union epoll_data {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;
struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 0x80000

#ifdef __cplusplus
extern "C" {
#endif
int winapk_epoll_create1(int flags);
int winapk_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int winapk_epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
#ifdef __cplusplus
}
#endif
#define epoll_create1 winapk_epoll_create1
#define epoll_ctl winapk_epoll_ctl
#define epoll_wait winapk_epoll_wait
