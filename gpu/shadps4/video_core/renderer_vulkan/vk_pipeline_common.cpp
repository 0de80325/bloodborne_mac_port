// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "bbport_toggles.h"
#include "common/thread.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
// Stats in nanoseconds (the arm64 cycle counter is 24 MHz before the M4).
u64 ElapsedNs(std::chrono::steady_clock::time_point start) {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count());
}
} // namespace

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    // A pipeline still in the creation pool must finish before its state goes away.
    if (!IsReady()) {
        WaitHandle();
    }
}

vk::Pipeline Pipeline::WaitHandle() const {
    if (building.load(std::memory_order_acquire) != 0) {
        const auto start = std::chrono::steady_clock::now();
        while (building.load(std::memory_order_acquire) != 0) {
            building.wait(1, std::memory_order_acquire);
        }
        BbStats::compile_wait_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
    }
    return *pipeline;
}

bool Pipeline::WaitReady(u64 max_ns) const {
    if (IsReady()) {
        return true;
    }
    // std::atomic::wait has no timeout; builds take milliseconds, so poll finely.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::nanoseconds(max_ns);
    bool ready = false;
    while (!(ready = IsReady()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    BbStats::compile_wait_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
    return ready;
}

void Pipeline::BuildAsync(std::function<void()> build) {
    if (!PipelineBuilder::Enabled()) {
        build();
        return;
    }
    building.store(1, std::memory_order_relaxed);
    PipelineBuilder::Submit([this, build = std::move(build)] {
        const auto start = std::chrono::steady_clock::now();
        // BB_COMPILE_DELAY_MS: test knob, slows every build as a cold Metal compiler cache does.
        static const int delay_ms = [] {
            const char* value = std::getenv("BB_COMPILE_DELAY_MS");
            return value ? std::atoi(value) : 0;
        }();
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        build();
        BbStats::background_build_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
        BbStats::background_builds.fetch_add(1, std::memory_order_relaxed);
        building.store(0, std::memory_order_release);
        building.notify_all();
    });
}

namespace PipelineBuilder {
namespace {
struct Pool {
    std::mutex mutex;
    std::condition_variable work_cv, idle_cv;
    std::deque<std::function<void()>> jobs;
    u32 running = 0;
    std::vector<std::jthread> threads;

    explicit Pool(u32 count) {
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this, i](std::stop_token stop) { Run(stop, i); });
        }
    }
    ~Pool() {
        for (auto& thread : threads) {
            thread.request_stop();
        }
        work_cv.notify_all();
    }

    void Run(std::stop_token stop, u32 index) {
        Common::SetCurrentThreadName(("bb:PipeBuild" + std::to_string(index)).c_str());
#ifdef __APPLE__
        // Below the render threads: mostly the efficiency cores.
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        std::unique_lock lk{mutex};
        while (true) {
            work_cv.wait(lk, [&] { return stop.stop_requested() || !jobs.empty(); });
            if (stop.stop_requested() && jobs.empty()) {
                return;
            }
            auto job = std::move(jobs.front());
            jobs.pop_front();
            ++running;
            lk.unlock();
            job();
            lk.lock();
            --running;
            if (jobs.empty() && running == 0) {
                idle_cv.notify_all();
            }
        }
    }
};

u32 ThreadCount() {
    static const u32 count = [] {
        const char* env = std::getenv("BB_PIPELINE_THREADS");
        return env ? u32(std::clamp(std::atoi(env), 0, 16)) : 4u;
    }();
    return count;
}

Pool& GetPool() {
    static Pool pool{ThreadCount()};
    return pool;
}
} // namespace

bool Enabled() {
    return ThreadCount() != 0;
}

void Submit(std::function<void()> job) {
    auto& pool = GetPool();
    {
        std::scoped_lock lk{pool.mutex};
        pool.jobs.push_back(std::move(job));
    }
    pool.work_cv.notify_one();
}

void Drain() {
    if (!Enabled()) {
        return;
    }
    auto& pool = GetPool();
    std::unique_lock lk{pool.mutex};
    pool.idle_cv.wait(lk, [&] { return pool.jobs.empty() && pool.running == 0; });
}
} // namespace PipelineBuilder


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
