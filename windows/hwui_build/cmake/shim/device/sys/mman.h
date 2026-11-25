// mmap of an ashmem region (JankTracker's, ProfileDataContainer's shared frame stats): never asked
// for here, so it fails as on a process with no such region.
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void*)-1)
static inline void* mmap(void*, size_t, int, int, int, long) { return MAP_FAILED; }
static inline int munmap(void*, size_t) { return 0; }
