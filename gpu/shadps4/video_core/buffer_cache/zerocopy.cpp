// bbport: zero-copy buffer import (Apple only, BB_ZERO_COPY=1). See zerocopy.h.
//
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef __APPLE__

#include "video_core/buffer_cache/zerocopy.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_split.h"
#include "host/bbgpu_protocol.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern "C" bool host_view_span(uint64_t start, uint64_t end, uint64_t* phys_out,
                               uint64_t* view_start_out, uint64_t* view_end_out);

namespace VideoCore {
namespace {

constexpr u64 kPage = BBGPU_GUEST_PAGE; // mirror views are page multiples
constexpr u64 kMaxImport = 1ull << 30;  // larger ranges stay on the arena path
constexpr size_t kMaxEntries = 256;
constexpr u64 kMaxBytes = 2ull << 30; // evictions keep the newest; single imports
                                     // are capped below so a fresh entry survives

bool ZeroCopyWanted() {
    static const int wanted = [] {
        const char* env = std::getenv("BB_ZERO_COPY");
        return env && env[0] == '1' ? 1 : 0;
    }();
    return wanted != 0;
}

void LogOnce(bool& done, const char* format, ...) {
    if (done) {
        return;
    }
    done = true;
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    std::printf("ZeroCopy: %s\n", line);
}

} // namespace

ZeroCopy::ZeroCopy(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

const Buffer* ZeroCopy::Obtain(BufferCache* owner, VAddr addr, u32 size, bool* fresh_out) {
    if (fresh_out) {
        *fresh_out = false;
    }
    if (!ZeroCopyWanted() || !BbSplit::Host() || !size || !owner) {
        return nullptr;
    }
    u64 phys = 0, vs = 0, ve = 0;
    if (!host_view_span(addr, u64(addr) + size, &phys, &vs, &ve)) {
        ++fallbacks;
        static bool logged = false;
        LogOnce(logged, "no single view covers 0x%llx+0x%x (arena fallback)",
                (unsigned long long)addr, size);
        return nullptr;
    }
    // Import the view-clipped aligned range: padding outside the view is never
    // bound (descriptors stay within the request), and clipping keeps imports
    // shared across requests in the same view.
    const u64 istart = std::max<u64>(addr & ~(kPage - 1), vs);
    const u64 iend = std::min<u64>((u64(addr) + size + kPage - 1) & ~(kPage - 1), ve);
    if (iend <= istart || iend - istart > kMaxImport) {
        return nullptr;
    }
    const u64 iphys = phys - (addr - istart); // istart <= addr, same view: contiguous
    std::scoped_lock lock{mutex};
    SweepRetired();
    const auto key = std::make_pair(istart, iend - istart);
    if (const auto it = index.find(key);
        it != index.end() && it->second->phys == iphys && it->second->addr == istart) {
        ++hits;
        lru.splice(lru.begin(), lru, it->second);
        return &it->second->buffer;
    }
    // Containment: an overlapping import of the same pages serves as well.
    for (const auto& [k, lit] : index) {
        const u64 s = lit->addr, e = s + lit->size;
        if (s <= istart && iend <= e && lit->phys + (istart - s) == iphys) {
            ++hits;
            lru.splice(lru.begin(), lru, lit);
            return &lit->buffer;
        }
    }
    const Buffer* imported = Import(owner, istart, iend - istart, iphys);
    if (!imported) {
        ++fallbacks;
    } else if (fresh_out) {
        *fresh_out = true;
    }
    return imported;
}

const Buffer* ZeroCopy::Import(BufferCache* owner, VAddr addr, u64 span, u64 phys) {
    const u64 start = addr;
    const vk::Device dev = instance.GetDevice();

    vk::BufferCreateInfo bci;
    bci.size = span;
    bci.usage = AllFlags;
    bci.sharingMode = vk::SharingMode::eExclusive;
    vk::Buffer buf{};
    if (dev.createBuffer(&bci, nullptr, &buf) != vk::Result::eSuccess || !buf) {
        return nullptr;
    }
    const vk::MemoryRequirements req = dev.getBufferMemoryRequirements(buf);
    // MoltenVK's unified types are all host-visible; prefer device-local.
    // (No vkGetMemoryHostPointerPropertiesEXT: the hpp dispatcher entry is
    // null when the extension isn't enabled on the device. A wrong pick fails
    // loudly at vkAllocateMemory and falls back to the arena path.)
    const u32 allowed = req.memoryTypeBits;
    const vk::PhysicalDeviceMemoryProperties mem_props =
        instance.GetPhysicalDevice().getMemoryProperties();
    // Imported memory must be host-visible; on MoltenVK that is the shared
    // (non-device-local) type. Pick the first host-visible set bit.
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        dev.destroyBuffer(buf, nullptr);
        return nullptr;
    }
    vk::ImportMemoryHostPointerInfoEXT import;
    import.handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
    import.pHostPointer = reinterpret_cast<void*>(start);
    vk::MemoryAllocateInfo ai;
    ai.pNext = &import;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    vk::DeviceMemory mem{};
    if (dev.allocateMemory(&ai, nullptr, &mem) != vk::Result::eSuccess || !mem) {
        dev.destroyBuffer(buf, nullptr);
        return nullptr;
    }
    if (dev.bindBufferMemory(buf, mem, 0) != vk::Result::eSuccess) {
        dev.freeMemory(mem, nullptr);
        dev.destroyBuffer(buf, nullptr);
        return nullptr;
    }
    vk::BufferDeviceAddressInfo bda_info;
    bda_info.buffer = buf;
    const vk::DeviceAddress bda = dev.getBufferAddress(&bda_info);
    if (!bda) {
        // Shaders address arena buffers by BDA; without one fall back.
        dev.freeMemory(mem, nullptr);
        dev.destroyBuffer(buf, nullptr);
        return nullptr;
    }

    Entry entry;
    entry.key = {start, span};
    entry.addr = start;
    entry.size = span;
    entry.phys = phys;
    entry.buffer.cpu_addr = start;
    entry.buffer.size_bytes = static_cast<size_t>(span);
    entry.buffer.mapped_data = std::span<u8>{reinterpret_cast<u8*>(start), static_cast<size_t>(span)};
    entry.buffer.is_coherent = true;
    entry.buffer.is_imported = true;
    entry.buffer.mem_type = MemoryType::DeviceLocal;
    entry.buffer.buffer.device = instance.GetDevice();
    entry.buffer.buffer.allocator = VK_NULL_HANDLE;
    entry.buffer.buffer.buffer = buf;
    entry.buffer.buffer.bda_addr = bda;
    entry.buffer.buffer.imported_memory = mem;

    EvictOverlap(owner, start, span, phys);
    lru.push_front(std::move(entry));
    auto it = lru.begin();
    index[{start, span}] = it;
    bytes += span;
    ++imports;
    EnforceCaps(owner);
    static bool logged = false;
    LogOnce(logged, "importing pool range 0x%llx+0x%llx (import #%llu)",
            (unsigned long long)start, (unsigned long long)span,
            (unsigned long long)imports.load());
    return &it->buffer;
}

void ZeroCopy::EvictOverlap(BufferCache* owner, VAddr addr, u64 size, u64 phys) {
    const u64 end = addr + size;
    for (auto it = index.begin(); it != index.end();) {
        const u64 s = it->first.first, e = s + it->first.second;
        const u64 ostart = std::max<u64>(s, addr), oend = std::min<u64>(e, end);
        // Disjoint, or the same pages (same phys chain: both imports read the
        // same shared bytes): keep. Only a remap conflicts.
        if (ostart >= oend ||
            it->second->phys + (ostart - it->second->addr) == phys + (ostart - addr)) {
            ++it;
            continue;
        }
        owner->DropResident(s, e - s);
        Retire(it->second);
        it = index.erase(it);
    }
}

void ZeroCopy::Retire(std::list<Entry>::iterator it) {
    bytes -= it->size;
    ++evicts;
    retired.push_back(Retired{it->buffer.buffer.buffer, it->buffer.buffer.imported_memory,
                              scheduler.CurrentTick()});
    // Prevent double-free: the list entry is gone; clear its handles first.
    it->buffer.buffer.buffer = vk::Buffer{};
    it->buffer.buffer.imported_memory = vk::DeviceMemory{};
    lru.erase(it);
}

void ZeroCopy::SweepRetired() {
    for (;;) {
        bool waiting = false;
        for (size_t i = 0; i < retired.size();) {
            if (scheduler.IsFree(retired[i].tick)) {
                const vk::Device dev = instance.GetDevice();
                dev.destroyBuffer(retired[i].buffer, nullptr);
                dev.freeMemory(retired[i].memory, nullptr);
                retired[i] = retired.back();
                retired.pop_back();
            } else {
                ++i;
            }
        }
        if (retired.size() <= 512) {
            return;
        }
        // Extreme pressure: wait for the oldest batch rather than leaking.
        if (!waiting) {
            scheduler.Wait(retired.front().tick);
            waiting = true;
        } else {
            return; // still busy after a wait: leave the rest for next time
        }
    }
}

void ZeroCopy::EnforceCaps(BufferCache* owner) {
    while ((lru.size() > kMaxEntries || bytes > kMaxBytes) && !lru.empty()) {
        auto it = std::prev(lru.end());
        index.erase(it->key);
        owner->DropResident(it->addr, it->size);
        Retire(it);
    }
}

} // namespace VideoCore

// BufferCache integration (resident BDA coverage lives here).
namespace VideoCore {

void BufferCache::DropResident(VAddr addr, u64 size) {
    if (!size) {
        return;
    }
    const u64 first = addr >> block_shift;
    const u64 last = (addr + size - 1) >> block_shift;
    std::vector<Backing> kept;
    for (const auto& backing : resident_ranges) {
        const u64 s = backing.start, e = backing.end;
        if (e <= first || s > last) {
            kept.push_back(backing);
            continue;
        }
        if (s < first) {
            kept.push_back(backing.SubRange(s, first));
        }
        if (e > last + 1) {
            kept.push_back(backing.SubRange(last + 1, e));
        }
    }
    resident_ranges.Clear();
    for (const auto& backing : kept) {
        resident_ranges.Add(backing);
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainImported(VAddr addr, u32 size) {
    if (!zerocopy) {
        zerocopy = std::make_unique<ZeroCopy>(instance, scheduler);
    }
    bool fresh = false;
    const Buffer* imported = zerocopy->Obtain(this, addr, size, &fresh);
    if (!imported) {
        return {nullptr, 0};
    }
    if (fresh) {
        // The import replaces any previous coverage (remap): refill BDA below.
        DropResident(addr, size);
        EnsureResident(imported, addr >> block_shift, (addr + size - 1) >> block_shift);
        runtime.RegisterHoistTarget(imported, imported->size_bytes);
    }
    return {imported, imported->Offset(addr)};
}

} // namespace VideoCore

#endif // __APPLE__
