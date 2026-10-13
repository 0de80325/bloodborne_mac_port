// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <atomic>
#include <functional>

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    /// Only for a ready pipeline (IsReady, or after WaitHandle).
    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    /// bbport: false while the creation pool is still building the pipeline.
    bool IsReady() const noexcept {
        return building.load(std::memory_order_acquire) == 0;
    }

    /// bbport: the handle once the creation pool has built it; null when the driver rejected it.
    vk::Pipeline WaitHandle() const;

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    /// bbport: runs `build` (the vkCreate*Pipelines call, where MoltenVK and Metal compile) on
    /// the pipeline creation pool, or right here when the pool is off.
    void BuildAsync(std::function<void()> build);

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    mutable std::atomic<u32> building{0};
};

/// bbport: background pipeline creation. Shader translation stays on the GPU command thread;
/// creating the Vulkan pipeline (MoltenVK converts SPIR-V to MSL and Metal compiles it) runs on
/// a few low-priority threads, so new pipelines compile in parallel and off the render path.
/// BB_PIPELINE_THREADS=n (default 4; 0 creates pipelines where they are needed, as before).
namespace PipelineBuilder {
bool Enabled();
void Submit(std::function<void()> job);
/// Waits until every submitted pipeline is built.
void Drain();
} // namespace PipelineBuilder

} // namespace Vulkan
