/* bbgpu-host internals shared by host/main.c and host/host_runtime.c.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "bbgpu_protocol.h"
#include "bbgpu_shared.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* One line to stdout, prefixed "bbgpu-host: " (one write: the runtime shares the log). */
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* The shared control block (mapped once the handshake is done). */
extern BbGpuShared *g_shared;

/* Guest view mirror (host_runtime.c). */
void host_runtime_init(mach_port_t guest_entry, uint64_t guest_entry_size);
void host_view_apply(uint32_t op, uint32_t prot, uint64_t address, uint64_t size, uint64_t phys);
int host_view_covered(uint64_t start, uint64_t end);
void host_view_stats(uint64_t *count, uint64_t *bytes, uint64_t *applied, uint64_t *failed);

/* End (exclusive) of the host's PROT_NONE reservation of the guest range
 * (main.c): views at or above it fail gracefully instead of replacing host
 * mappings. 0 before the reservation is made. */
uint64_t host_reservation_end(void);

/* Zero-copy (buffer_cache/zerocopy.cpp): pool offset of the single mirror view
 * fully covering [start, end), plus that view's bounds, or false (no view,
 * several views, or the range is not covered). The caller revalidates on every
 * use; entries keyed by it stay correct without unmap wiring. */
bool host_view_span(uint64_t start, uint64_t end, uint64_t* phys_out, uint64_t* view_start_out,
                    uint64_t* view_end_out);
