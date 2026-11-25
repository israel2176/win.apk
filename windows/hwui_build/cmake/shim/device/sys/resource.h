#pragma once
#define PRIO_PROCESS 0
static inline int setpriority(int, int, int) { return 0; }
static inline int getpriority(int, int) { return 0; }
