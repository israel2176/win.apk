// sys/eventfd.h on hwui_glue's emulated descriptors (fd_emul.cpp). A translation unit that
// includes it reads, writes and closes through them (Looper.cpp: its wake fd; read, write and
// close of any other descriptor go to the CRT's).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <io.h>

#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000
#define EFD_SEMAPHORE 1

#ifdef __cplusplus
extern "C" {
#endif
int winapk_eventfd(unsigned int initval, int flags);
long long winapk_fd_read(int fd, void* buf, size_t n);
long long winapk_fd_write(int fd, const void* buf, size_t n);
int winapk_fd_close(int fd);
#ifdef __cplusplus
}
#endif
#define eventfd winapk_eventfd
#define read winapk_fd_read
#define write winapk_fd_write
#define close winapk_fd_close
