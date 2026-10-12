/* bbgpu-host: the native arm64 GPU process (docs/DESIGN_NATIVE_GPU_PROCESS.md), milestone M2.
 * Spawned by bb-probe (src/gpu_process.c) with BB_GPU_PROCESS=1. It reserves the guest range,
 * maps the guest memory entry it receives in the handshake, mirrors every guest view at the same
 * address as the runtime and answers verification requests (host/bbgpu_protocol.h). Rendering
 * moves here in M3. With arm64 SDL3 (deps/arm64) it also opens a window on the main thread.
 * It exits when bb-probe exits or execs, or when the control port dies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "bbgpu_protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <mach/mach_vm.h>
#include <mach/notify.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#if BBGPU_HOST_SDL
#include <SDL3/SDL.h>
#endif

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
    char line[512];
    int n = snprintf(line, sizeof line, "bbgpu-host: ");
    va_list args;
    va_start(args, format);
    n += vsnprintf(line + n, sizeof line - (size_t)n, format, args);
    va_end(args);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    (void)!write(1, line, (size_t)n); /* one write per line: the runtime shares the log */
}

static mach_port_t host_port = MACH_PORT_NULL, ctl_port = MACH_PORT_NULL, entry = MACH_PORT_NULL;
static uint64_t entry_size;
static pid_t parent_pid;
static int quit_code = -1; /* >= 0: the main thread exits with it */

static void finish(int code, const char *why) {
    say("exiting: %s", why);
    __atomic_store_n(&quit_code, code, __ATOMIC_RELEASE);
}

/* ---- the mirror: guest views at the runtime's addresses -------------------------------- */
typedef struct { uint64_t start, end, phys; uint32_t prot; } View;
static View *views;
static size_t view_count, view_capacity;
static uint64_t events_applied, failures, view_bytes;

static size_t view_index(uint64_t a) { /* first view with end > a */
    size_t lo = 0, hi = view_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (views[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
static void view_insert(size_t at, View v) {
    if (view_count == view_capacity) {
        view_capacity = view_capacity ? view_capacity * 2 : 256;
        views = realloc(views, view_capacity * sizeof *views);
        if (!views) { say("out of memory"); _exit(1); }
    }
    memmove(views + at + 1, views + at, (view_count - at) * sizeof *views);
    views[at] = v;
    ++view_count;
    view_bytes += v.end - v.start;
}
static void split_at(uint64_t a) {
    size_t i = view_index(a);
    if (i == view_count || views[i].start >= a) return;
    View right = views[i];
    right.phys += a - right.start;
    right.start = a;
    views[i].end = a;
    view_bytes -= right.end - right.start; /* view_insert adds it back */
    view_insert(i + 1, right);
}
static void drop(uint64_t start, uint64_t end) {
    split_at(start);
    split_at(end);
    size_t i = view_index(start), n = 0;
    while (i + n < view_count && views[i + n].start < end) view_bytes -= views[i + n].end - views[i + n].start, ++n;
    memmove(views + i, views + i + n, (view_count - i - n) * sizeof *views);
    view_count -= n;
}
static int covered(uint64_t start, uint64_t end) {
    uint64_t at = start;
    for (size_t i = view_index(start); at < end; ++i) {
        if (i == view_count || views[i].start > at) return 0;
        at = views[i].end;
    }
    return 1;
}

static void fail_event(const BbGpuEvent *e, const char *what, int code) {
    if (failures++ < 10)
        say("event %u at 0x%" PRIx64 "+0x%" PRIx64 " (phys 0x%" PRIx64 "): %s failed (%d)", e->op, e->address, e->size,
            e->phys, what, code);
}

/* The host's views stay read/write: guest protection only matters where the game runs
 * (design 3.4); it is recorded per view, which keeps the view table identical to the runtime's. */
static void apply(const BbGpuEvent *e) {
    ++events_applied;
    const uint64_t start = e->address, end = e->address + e->size;
    if (!e->size || (start | e->size) % BBGPU_GUEST_PAGE || start < BBGPU_GUEST_MIN || end > BBGPU_GUEST_MAX ||
        end < start) {
        fail_event(e, "range check", EINVAL);
        return;
    }
    switch (e->op) {
    case BBGPU_EV_MAP: {
        if (e->phys % BBGPU_GUEST_PAGE || e->phys > entry_size || e->size > entry_size - e->phys) {
            fail_event(e, "entry range check", EINVAL);
            return;
        }
        mach_vm_address_t at = start;
        kern_return_t kr = mach_vm_map(mach_task_self(), &at, e->size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry,
                                       e->phys, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
                                       VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS || at != start) { fail_event(e, "mach_vm_map", kr); return; }
        drop(start, end);
        view_insert(view_index(start), (View){start, end, e->phys, e->prot});
        break;
    }
    case BBGPU_EV_UNMAP:
        /* back to the reservation, as the runtime does */
        if (mmap((void *)start, e->size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED) {
            fail_event(e, "mmap(PROT_NONE)", errno);
            return;
        }
        drop(start, end);
        break;
    case BBGPU_EV_PROTECT:
        split_at(start);
        split_at(end);
        for (size_t i = view_index(start); i < view_count && views[i].start < end; ++i) views[i].prot = e->prot;
        break;
    default:
        fail_event(e, "unknown op", EINVAL);
    }
}

static void verify(const BbGpuVerify *v) {
    BbGpuVerifyReply r;
    memset(&r, 0, sizeof r);
    r.seq = v->seq;
    r.views = view_count;
    r.bytes = view_bytes;
    r.events = events_applied;
    r.failures = failures;
    r.count = v->count <= BBGPU_SAMPLES_MAX ? v->count : BBGPU_SAMPLES_MAX;
    for (uint32_t i = 0; i < r.count; ++i) {
        const BbGpuSample *s = &v->samples[i];
        if (s->size % 8 || s->address + s->size < s->address || !covered(s->address, s->address + s->size)) continue;
        r.mapped[i] = 1; /* read through the mirror, at the runtime's address */
        r.checksums[i] = bbgpu_checksum(BBGPU_CHECKSUM_SEED, (const void *)(uintptr_t)s->address, s->size);
    }
    if (v->events != events_applied)
        say("verify #%" PRIu64 ": runtime sent %" PRIu64 " events, %" PRIu64 " applied", v->seq, v->events, events_applied);
    r.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    r.h.msgh_size = sizeof r;
    r.h.msgh_remote_port = ctl_port;
    r.h.msgh_id = BBGPU_MSG_VERIFY_REPLY;
    kern_return_t kr = mach_msg(&r.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof r, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) finish(1, mach_error_string(kr));
}

typedef union {
    mach_msg_header_t h;
    BbGpuSetup setup;
    BbGpuEvents events;
    BbGpuVerify verify;
    mach_dead_name_notification_t dead;
} Message;
typedef struct { Message m; mach_msg_max_trailer_t trailer; } MessageBuffer;

static void *receiver(void *unused) {
    (void)unused;
    static MessageBuffer in;
    for (;;) {
        kern_return_t kr = mach_msg(&in.m.h, MACH_RCV_MSG, 0, sizeof in, host_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) { finish(1, mach_error_string(kr)); return NULL; }
        switch (in.m.h.msgh_id) {
        case BBGPU_MSG_EVENTS:
            if (in.m.events.count <= BBGPU_EVENTS_MAX &&
                in.m.h.msgh_size >= offsetof(BbGpuEvents, events) + in.m.events.count * sizeof(BbGpuEvent))
                for (uint32_t i = 0; i < in.m.events.count; ++i) apply(&in.m.events.events[i]);
            break;
        case BBGPU_MSG_VERIFY:
            if (in.m.h.msgh_size >= sizeof(BbGpuVerify)) verify(&in.m.verify);
            break;
        case BBGPU_MSG_SHUTDOWN:
            finish(0, "shutdown requested");
            return NULL;
        case MACH_NOTIFY_DEAD_NAME:
            finish(0, "control port died (bb-probe exited)");
            return NULL;
        default:
            mach_msg_destroy(&in.m.h);
        }
    }
}

/* bb-probe exiting or exec'ing (runtime_restart) ends the host too. */
static void *parent_watch(void *unused) {
    (void)unused;
    int kq = kqueue();
    struct kevent change, event;
    EV_SET(&change, parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXEC, 0, NULL);
    if (kq < 0 || kevent(kq, &change, 1, &event, 1, NULL) < 0 || (event.flags & EV_ERROR)) {
        finish(0, "bb-probe is gone");
        return NULL;
    }
    finish(0, event.fflags & NOTE_EXEC ? "bb-probe exec'd (restart)" : "bb-probe exited");
    return NULL;
}

static int translated(void) {
    int value = 0;
    size_t size = sizeof value;
    return sysctlbyname("sysctl.proc_translated", &value, &size, NULL, 0) == 0 && value;
}

static int handshake(mach_port_t *parent) {
    const char *marker = getenv("BBGPU_HANDSHAKE");
    if (!marker || atoi(marker) != getppid()) {
        say("not started by bb-probe (run the game with BB_GPU_PROCESS=1)");
        return -1;
    }
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t n = 0;
    kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &n);
    if (kr != KERN_SUCCESS || n < 1 || ports[0] == MACH_PORT_NULL) {
        say("no handshake port inherited");
        return -1;
    }
    *parent = ports[0];
    for (mach_msg_type_number_t i = 1; i < n; ++i) if (ports[i]) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, n * sizeof *ports);
    mach_ports_register(mach_task_self(), NULL, 0); /* not passed on to anything this spawns */

    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &host_port) != KERN_SUCCESS) return -1;
    mach_port_limits_t limits = {MACH_PORT_QLIMIT_LARGE};
    mach_port_set_attributes(mach_task_self(), host_port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
                             MACH_PORT_LIMITS_INFO_COUNT);
    BbGpuHello hello;
    memset(&hello, 0, sizeof hello);
    hello.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND);
    hello.h.msgh_size = sizeof hello;
    hello.h.msgh_remote_port = *parent;
    hello.h.msgh_local_port = host_port;
    hello.h.msgh_id = BBGPU_MSG_HELLO;
    hello.version = BBGPU_PROTOCOL_VERSION;
    hello.pid = (uint32_t)getpid();
    kr = mach_msg(&hello.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof hello, 0, MACH_PORT_NULL, 5000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) { say("hello: %s", mach_error_string(kr)); return -1; }

    static MessageBuffer in;
    for (;;) {
        kr = mach_msg(&in.m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in, host_port, 10000, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) { say("setup: %s", mach_error_string(kr)); return -1; }
        if (in.m.h.msgh_id == BBGPU_MSG_SETUP) break;
        mach_msg_destroy(&in.m.h);
    }
    const BbGpuSetup *s = &in.m.setup;
    if (!(s->h.msgh_bits & MACH_MSGH_BITS_COMPLEX) || s->body.msgh_descriptor_count != 2 || s->version != BBGPU_PROTOCOL_VERSION ||
        s->guest_min != BBGPU_GUEST_MIN || s->guest_max != BBGPU_GUEST_MAX) {
        say("bad setup message (protocol %u)", s->version);
        mach_msg_destroy(&in.m.h);
        return -1;
    }
    entry = s->entry.name;
    ctl_port = s->control.name;
    entry_size = s->entry_size;
    parent_pid = (pid_t)s->pid;
    return 0;
}

static void send_ready(int status, int window) {
    BbGpuReady r;
    memset(&r, 0, sizeof r);
    r.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    r.h.msgh_size = sizeof r;
    r.h.msgh_remote_port = ctl_port;
    r.h.msgh_id = BBGPU_MSG_READY;
    r.pid = (uint32_t)getpid();
    r.page_size = (uint32_t)getpagesize();
    r.native = !translated();
    r.window = window;
    r.status = status;
    kern_return_t kr = mach_msg(&r.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof r, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) say("ready: %s", mach_error_string(kr));
}

int main(void) {
    say("pid %d, %s, page %d bytes", (int)getpid(), translated() ? "TRANSLATED (Rosetta)" : "native arm64", getpagesize());
    /* The guest range first, before anything (SDL, Metal) maps memory there. The kernel hole
     * below 0x7000000000 is the same in arm64 processes. */
    int status = 0;
    const uint64_t span = BBGPU_GUEST_MAX - BBGPU_GUEST_MIN;
    void *reserved = mmap((void *)BBGPU_GUEST_MIN, span, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (reserved != (void *)BBGPU_GUEST_MIN) {
        say("cannot reserve the guest range 0x%" PRIx64 "-0x%" PRIx64 " (got %p)", BBGPU_GUEST_MIN, BBGPU_GUEST_MAX, reserved);
        if (reserved != MAP_FAILED) munmap(reserved, span);
        status = ENOMEM;
    }
    mach_port_t parent = MACH_PORT_NULL;
    if (handshake(&parent)) return 1;
    mach_port_deallocate(mach_task_self(), parent); /* the handshake port; control goes through ctl_port */
    say("handshake done: parent pid %d, guest entry 0x%" PRIx64 " bytes, guest range 0x%" PRIx64 "-0x%" PRIx64 " reserved",
        (int)parent_pid, entry_size, BBGPU_GUEST_MIN, BBGPU_GUEST_MAX);
    /* The entry must be mappable here (send right valid, size as announced). */
    if (!status) {
        mach_vm_address_t at = 0;
        kern_return_t kr = mach_vm_map(mach_task_self(), &at, BBGPU_GUEST_PAGE, 0, VM_FLAGS_ANYWHERE, entry,
                                       entry_size - BBGPU_GUEST_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                                       VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { say("cannot map the guest entry: %s", mach_error_string(kr)); status = kr; }
        else mach_vm_deallocate(mach_task_self(), at, BBGPU_GUEST_PAGE);
    }
    mach_port_t previous = MACH_PORT_NULL;
    if (mach_port_request_notification(mach_task_self(), ctl_port, MACH_NOTIFY_DEAD_NAME, 0, host_port,
                                       MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous) != KERN_SUCCESS) {
        say("bb-probe's control port is already dead");
        return 0;
    }
    pthread_t thread;
    pthread_create(&thread, NULL, parent_watch, NULL);
    pthread_detach(thread);

    int window = 0;
#if BBGPU_HOST_SDL
    /* Cocoa: SDL video and the window live on the main thread. */
    SDL_Window *sdl_window = NULL;
    if (!status) {
        if (!SDL_Init(SDL_INIT_VIDEO)) say("SDL_Init: %s", SDL_GetError());
        else if (!(sdl_window = SDL_CreateWindow("Bloodborne GPU host (M2)", 640, 360, SDL_WINDOW_RESIZABLE)))
            say("SDL_CreateWindow: %s", SDL_GetError());
        window = sdl_window ? 1 : -1;
        if (sdl_window) say("window open (SDL %d.%d.%d, video driver %s)", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                            SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver());
    }
#endif
    pthread_create(&thread, NULL, receiver, NULL);
    pthread_detach(thread);
    send_ready(status, window);
    if (status) { say("setup failed; exiting"); return 1; }
    say("ready: mirroring guest views");

    while (__atomic_load_n(&quit_code, __ATOMIC_ACQUIRE) < 0) {
#if BBGPU_HOST_SDL
        SDL_Event event;
        if (sdl_window) {
            while (SDL_WaitEventTimeout(&event, 100))
                if (__atomic_load_n(&quit_code, __ATOMIC_ACQUIRE) >= 0) break;
            continue;
        }
#endif
        usleep(100000);
    }
    say("mirrored %zu views (%.2f GiB), %" PRIu64 " events applied, %" PRIu64 " failures", view_count,
        (double)view_bytes / (1u << 30), events_applied, failures);
#if BBGPU_HOST_SDL
    if (sdl_window) SDL_DestroyWindow(sdl_window);
    SDL_Quit();
#endif
    return __atomic_load_n(&quit_code, __ATOMIC_ACQUIRE);
}
