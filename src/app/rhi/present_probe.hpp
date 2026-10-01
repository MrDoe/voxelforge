#pragma once

// The reporting surface of the swapchain acquire/present probe.
//
// Deliberately NOT part of the App interface: frame/run.cpp calls
// forceFrameResults() on the headless path (which never acquires a real
// swapchain image, so a hook placed only beside the real present would be
// unreachable from every test) and reportFrameResults() after the real
// present.

#include <vulkan/vulkan.h>

namespace vf {
namespace app {

void reportFrameResult(VkResult r, const char* what, unsigned long long frame);

void forceFrameResults(unsigned long long frame);

} // namespace app
} // namespace vf
