// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the two-process split of the GPU library (bbport_split.h).
#include "bbport_split.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include "common/thread.h"
#include "core/platform.h"

namespace Libraries::VideoOut {
void BbSplitApplyEvent(const BbGpuRecord* record);
}

namespace BbSplit {
namespace {

std::atomic<Mode> g_mode{Mode::Single};
BbGpuShared* g_shared;
std::mutex g_push_mutex; // producers on this side of each ring

constexpr u64 Second = 1'000'000'000;

/// The peer process is gone: nothing can make progress any more.
[[noreturn]] void PeerLost(const char* what) {
    std::printf("GPU split: the %s process is gone; exiting\n", what);
    std::fflush(stdout);
    _exit(1);
}

bool PeerAlive(s32 pid) {
    return pid <= 0 || kill(pid, 0) == 0;
}

void PushTo(BbGpuRing& ring, const void* record, s32 peer_pid, const char* peer) {
    std::scoped_lock lock{g_push_mutex};
    for (u32 waited = 0;; ++waited) {
        if (bbgpu_ring_push(g_shared, &ring, record, Second) == 0) {
            return;
        }
        if (!PeerAlive(peer_pid)) {
            PeerLost(peer);
        }
        if (waited == 10) {
            std::printf("GPU split: the %s has not taken records for 10 s (ring full)\n", peer);
        }
    }
}

} // namespace

Mode GetMode() {
    return g_mode.load(std::memory_order_relaxed);
}

BbGpuShared* Shared() {
    return g_shared;
}

void Attach(BbGpuShared* shared) {
    g_shared = shared;
}

void InitRemote(BbGpuShared* shared) {
    g_shared = shared;
    g_mode = Mode::Remote;
}

void InitHost(BbGpuShared* shared) {
    g_shared = shared;
    g_mode = Mode::Host;
}

void Push(const void* record) {
    PushTo(g_shared->submit, record, g_shared->host_pid, "GPU host");
}

s64 Request(void* record, u32* slot) {
    const int claimed = bbgpu_reply_claim(g_shared);
    *slot = static_cast<u32>(claimed);
    Push(record);
    s64 result = 0;
    for (u32 waited = 0; bbgpu_reply_wait(g_shared, claimed, &result, Second) != 0; ++waited) {
        if (!PeerAlive(g_shared->host_pid)) {
            PeerLost("GPU host");
        }
        if (waited == 30) {
            std::printf("GPU split: request %u unanswered for 30 s\n",
                        static_cast<const BbGpuRecord*>(record)->type);
        }
    }
    return result;
}

void StartEventDispatcher() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:GpuEvents");
        BbGpuShared* s = g_shared;
        for (;;) {
            const BbGpuRecord* r = bbgpu_ring_peek(s, &s->events, Second);
            if (!r) {
                if (!PeerAlive(s->host_pid)) {
                    PeerLost("GPU host");
                }
                continue;
            }
            switch (r->type) {
            case BBGPU_EV_IRQ:
                // The game's own registrations (sceGnmAddEqEvent, the submission lock) run here
                // as they ran on the GPU thread in a single process.
                Platform::IrqC::Instance()->Signal(
                    static_cast<Platform::InterruptId>(reinterpret_cast<const BbGpuIrq*>(r)->id));
                break;
            case BBGPU_EV_FLIP:
            case BBGPU_EV_EOP_FLIP_QUEUED:
            case BBGPU_EV_VBLANK:
                Libraries::VideoOut::BbSplitApplyEvent(r);
                break;
            default:
                std::printf("GPU split: unknown event %u\n", r->type);
            }
            bbgpu_ring_pop(&s->events, r);
        }
    }).detach();
}

void Post(const void* record) {
    PushTo(g_shared->events, record, g_shared->game_pid, "game");
}

void ForwardInterrupts() {
    using Platform::InterruptId;
    static constexpr InterruptId ids[] = {
        InterruptId::Compute0RelMem, InterruptId::Compute1RelMem, InterruptId::Compute2RelMem,
        InterruptId::Compute3RelMem, InterruptId::Compute4RelMem, InterruptId::Compute5RelMem,
        InterruptId::Compute6RelMem, InterruptId::GfxEop,         InterruptId::GpuIdle,
    };
    static int uid; // one registration per interrupt
    for (const InterruptId id : ids) {
        Platform::IrqC::Instance()->Register(
            id,
            [](InterruptId irq) {
                const BbGpuIrq event{{BBGPU_EV_IRQ, sizeof(BbGpuIrq)}, static_cast<u32>(irq), 0};
                Post(&event);
            },
            &uid);
    }
}

} // namespace BbSplit
