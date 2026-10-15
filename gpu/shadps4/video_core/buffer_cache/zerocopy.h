// bbport: zero-copy buffer import (Apple only, BB_ZERO_COPY=1).
//
// Instead of copying pool ranges into the arena mirror, import the pool pages
// themselves as a VkBuffer (VK_EXT_external_memory_host) and bind that. The
// pool is 6 GiB and the arena 8 GiB virtual on a 12 GiB heap; serving binds
// from shared pages removes the duplication. Bytes stay correct without
// unmap wiring: every Obtain revalidates the pool offset through the mirror
// view table, so a remapped range misses and re-imports; evicted imports drop
// their resident BDA coverage so the next bind refills it.
//
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {
struct Buffer;
class BufferCache;

class ZeroCopy {
public:
    explicit ZeroCopy(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);

    /// Imported Buffer sharing the pool pages of [addr, addr+size), or nullptr
    /// (arena fallback). Thread-safe. The Buffer lives in the cache; callers
    /// must not destroy it. Evicted entries are retired until the GPU is done.
    /// fresh_out (when non-null) reports a new import needing BDA coverage.
    const Buffer* Obtain(BufferCache* owner, VAddr addr, u32 size, bool* fresh_out = nullptr);
    u64 imported_bytes() const {
        return bytes;
    }

private:
    struct Entry {
        Buffer buffer;
        std::pair<u64, u64> key{};
        u64 addr = 0, size = 0, phys = 0;
    };

    const Buffer* Import(BufferCache* owner, VAddr addr, u64 span, u64 phys);
    void EvictOverlap(BufferCache* owner, VAddr addr, u64 size, u64 phys);
    void Retire(std::list<Entry>::iterator it);
    void SweepRetired();
    void EnforceCaps(BufferCache* owner);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    std::mutex mutex;
    // Non-overlapping by guest address (insert drops overlappers).
    std::map<std::pair<u64, u64>, std::list<Entry>::iterator> index;
    std::list<Entry> lru;
    struct Retired {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        u64 tick;
    };
    std::vector<Retired> retired;
    u64 bytes = 0;
    std::atomic<u64> hits{0}, imports{0}, fallbacks{0}, evicts{0};
};

} // namespace VideoCore
