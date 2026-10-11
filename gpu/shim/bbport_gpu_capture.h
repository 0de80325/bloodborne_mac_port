// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one-frame Metal GPU capture to a .gputrace file (macOS), for Xcode's frame debugger and
// per-pass GPU timings. BB_GPU_CAPTURE=<seconds> captures the frame presented that long after
// the first present; BB_GPU_CAPTURE_FILE sets the output (default frame.gputrace). Needs
// METAL_CAPTURE_ENABLED=1 in the environment.
#pragma once

namespace BbGpuCapture {
#ifdef __APPLE__
/// Call right after each swapchain present.
void OnPresent();
#else
inline void OnPresent() {}
#endif
} // namespace BbGpuCapture
