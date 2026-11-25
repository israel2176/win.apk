// Only the types Looper.h names: Win.apk's Looper (winapk_looper.cpp) does not poll descriptors.
#pragma once
#include <stdint.h>
typedef union epoll_data { void* ptr; int fd; uint32_t u32; uint64_t u64; } epoll_data_t;
struct epoll_event { uint32_t events; epoll_data_t data; };
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
