// stdatomic.h for C++ (what libc++'s gives: the C11 names as std::atomic's), not clang's C
// header, whose macros break std::atomic_thread_fence under libstdc++.
#pragma once
#ifdef __cplusplus
#include <atomic>
using std::atomic_bool; using std::atomic_int; using std::atomic_uint; using std::atomic_long;
using std::atomic_int_least32_t; using std::atomic_uint_least32_t; using std::atomic_intptr_t;
using std::atomic_uintptr_t; using std::atomic_size_t; using std::atomic_int_fast32_t;
using std::memory_order; using std::memory_order_relaxed; using std::memory_order_acquire;
using std::memory_order_release; using std::memory_order_acq_rel; using std::memory_order_seq_cst;
using std::atomic_load_explicit; using std::atomic_store_explicit; using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub_explicit; using std::atomic_fetch_or_explicit; using std::atomic_fetch_and_explicit;
using std::atomic_compare_exchange_strong_explicit; using std::atomic_compare_exchange_weak_explicit;
using std::atomic_exchange_explicit; using std::atomic_thread_fence; using std::atomic_load; using std::atomic_store;
using std::atomic_init;
#else
#include_next <stdatomic.h>
#endif
