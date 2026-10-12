// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: CPU spin-wait hint and cycle counter, portable between the x86-64 build (game process
// under Rosetta 2) and the native arm64 GPU host (docs/DESIGN_NATIVE_GPU_PROCESS.md).
//
// x86-64: exactly the previous __builtin_ia32_pause() / __rdtsc().
// arm64:  `yield` and the virtual counter CNTVCT_EL0. Measured on the M4 Max (2026-10-11):
//         CNTFRQ_EL0 = 1 GHz (fixed by ARMv8.6+) and Rosetta's RDTSC returns the same
//         CNTVCT_EL0 value (same epoch and units), so cycle counts and guest TSC values agree
//         between the two processes. Only used for statistics here; guest-visible TSC reads
//         go through sceKernelReadTsc (shim/bbgpu.cpp).

#pragma once

#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace bbport {

static inline __attribute__((always_inline)) void CpuPause() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#else
#error "bbport::CpuPause: unsupported architecture"
#endif
}

static inline __attribute__((always_inline)) std::uint64_t CpuCycles() {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
#error "bbport::CpuCycles: unsupported architecture"
#endif
}

} // namespace bbport
