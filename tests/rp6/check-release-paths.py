"""Release lifetime and guest-dispatch fault injection against production methods."""
import hashlib
import json
from pathlib import Path
import subprocess
import re

from common import sdk, out as root, span, CXX
dispatcher = (sdk / "src/system/function_dispatcher.cpp").read_text()
presenter = (sdk / "src/ui/vulkan/vulkan_presenter.cpp").read_text()
tracker = (sdk / "src/ui/vulkan/vulkan_submission_tracker.cpp").read_text()


def extract(source, signature):
    start, end = span(source, signature)
    return source[start:end]


code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
#define REX_PLATFORM_ANDROID 1
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
#define REX_FATAL(...) throw std::runtime_error("fatal")
#define REXCVAR_GET(x) x
#define assert_true(x) assert(x)
bool guest_fatal_invalid_call = PRODUCTION_INVALID_CALL_DEFAULT;
union Reg { uint64_t u64; uint32_t u32; };
struct PPCContext { uint32_t last_indirect_target; uint64_t lr; Reg r3; Reg r1{}, r4{}; };
struct ThreadState {
  static ThreadState* Get() { return nullptr; }
  uint32_t thread_id() { return 1; }
};
using Handle = void*;
using VkDevice = Handle;
using VkFence = Handle;
using VkSwapchainKHR = Handle;
constexpr Handle VK_NULL_HANDLE = nullptr;
int destroyed_swapchains = 0, destroyed_fences = 0, destroyed_framebuffers = 0;
struct VulkanInstance {
  struct Functions {
    void vkDestroySurfaceKHR(Handle, Handle, void*) const {}
  } f;
  const Functions& functions() const { return f; }
  Handle instance() const { return nullptr; }
};
struct VulkanDevice {
  struct Functions {
    void vkDestroySwapchainKHR(Handle, Handle, void*) const { ++destroyed_swapchains; }
    void vkDestroyFramebuffer(Handle, Handle, void*) const { ++destroyed_framebuffers; }
    void vkDestroyImageView(Handle, Handle, void*) const {}
    void (*vkDestroyFence)(Handle, Handle, void*) = [](Handle, Handle, void*) { ++destroyed_fences; };
  } f;
  VulkanInstance i;
  const Functions& functions() const { return f; }
  Handle device() const { return nullptr; }
  const VulkanInstance* vulkan_instance() const { return &i; }
};
namespace util {
  template<class F> void DestroyAndNullHandle(F f, Handle dev, Handle& h) {
    if (h) { f(dev,h,nullptr); h=nullptr; }
  }
}
struct VulkanSubmissionTracker {
  VulkanDevice* vulkan_device_;
  bool completed = false;
  std::vector<Handle> fences_reclaimed_;
  std::deque<std::pair<uint64_t, Handle>> fences_pending_;
  Handle fence_acquired_ = nullptr;
  bool AwaitAllSubmissionsCompletion() { return completed; }
  void Shutdown();
};
struct VulkanPresenter {
  struct PaintContext {
    struct SwapchainFramebuffer { Handle framebuffer, image_view; };
    Handle swapchain = reinterpret_cast<Handle>(1);
    VulkanSubmissionTracker submission_tracker;
    VulkanDevice* vulkan_device;
    std::vector<SwapchainFramebuffer> swapchain_framebuffers, retired_framebuffers;
    std::vector<Handle> retired_present_semaphores, swapchain_present_semaphores, swapchain_images;
    struct { uint32_t width = 1280, height = 720; } swapchain_extent;
    uint32_t present_queue_family = 0;
    Handle vulkan_surface = reinterpret_cast<Handle>(2);
    Handle PrepareForSwapchainRetirement();
    void DestroySwapchainAndVulkanSurface();
  };
};
'''
code += extract(dispatcher, "bool RecoverKnownSkate3InvalidCallback(") + "\n"
code += extract(dispatcher, "static void InvalidFunctionTrap(") + "\n"
code += extract(presenter, "VkSwapchainKHR VulkanPresenter::PaintContext::PrepareForSwapchainRetirement()") + "\n"
code += extract(presenter, "void VulkanPresenter::PaintContext::DestroySwapchainAndVulkanSurface()") + "\n"
code += extract(tracker, "void VulkanSubmissionTracker::Shutdown()") + "\n"
default = re.search(r"REXCVAR_DEFINE_BOOL\(guest_fatal_invalid_call,\s*(true|false)", dispatcher)[1]
code = code.replace("PRODUCTION_INVALID_CALL_DEFAULT", default)
code += r'''
int main() {
  assert(guest_fatal_invalid_call); // The shipped default, not a harness override.
  PPCContext ctx{0xE7BF8FEF, 0x82012344, {0x82B463A8}};
  bool stopped = false;
  try { InvalidFunctionTrap(ctx, nullptr); } catch (const std::runtime_error&) { stopped = true; }
  assert(stopped);
  ctx.lr = 0x82B326C4;
  InvalidFunctionTrap(ctx, nullptr);
  assert(ctx.r3.u64 == 0);
  std::cout << "PASS: unknown guest target stops by default; known factory recovery remains available.\n";

  VulkanDevice device;
  VulkanPresenter::PaintContext context;
  context.vulkan_device = &device;
  context.submission_tracker.vulkan_device_ = &device;
  context.swapchain_framebuffers.push_back({reinterpret_cast<Handle>(3),reinterpret_cast<Handle>(4)});
  stopped = false;
  try { context.DestroySwapchainAndVulkanSurface(); } catch (const std::runtime_error&) { stopped = true; }
  assert(stopped && destroyed_framebuffers == 0 && destroyed_swapchains == 0);
  assert(context.swapchain && context.vulkan_surface && context.swapchain_framebuffers.size() == 1);
  context.submission_tracker.completed = true;
  context.DestroySwapchainAndVulkanSurface();
  assert(destroyed_framebuffers == 1 && destroyed_swapchains == 1);
  assert(!context.swapchain && !context.vulkan_surface && context.swapchain_framebuffers.empty());
  context.DestroySwapchainAndVulkanSurface();
  assert(destroyed_framebuffers == 1 && destroyed_swapchains == 1);
  std::cout << "PASS: failed completion preserves swapchain group; completed cleanup runs once.\n";

  VulkanSubmissionTracker pending;
  pending.vulkan_device_ = &device;
  pending.fences_pending_.push_back({1,reinterpret_cast<Handle>(5)});
  pending.fences_reclaimed_.push_back(reinterpret_cast<Handle>(6));
  stopped = false;
  try { pending.Shutdown(); } catch (const std::runtime_error&) { stopped = true; }
  assert(stopped && destroyed_fences == 0 && pending.fences_pending_.size() == 1);
  pending.completed = true;
  pending.Shutdown();
  assert(destroyed_fences == 2 && pending.fences_pending_.empty() && pending.fences_reclaimed_.empty());
  pending.Shutdown();
  assert(destroyed_fences == 2);
  std::cout << "PASS: failed shutdown frees no fences; completed cleanup is idempotent.\n";
}
'''

cpp = root / "release_paths.cpp"
exe = root / "release_paths"
cpp.write_text(code)
subprocess.run([*CXX, "-std=c++20", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", str(cpp), "-o", str(exe)], check=True)
run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
print(run.stdout, end="")
report = {
    "method": "Actual production methods, with mock driver completion and guest-call environment; ASAN + UBSAN",
    "limits": "Control-flow fault injection, not a physical Vulkan GPU or Android gameplay test",
    "output": run.stdout,
    "source_sha256": {
        name: hashlib.sha256(content.encode()).hexdigest()
        for name, content in [("function_dispatcher.cpp", dispatcher), ("vulkan_presenter.cpp", presenter), ("vulkan_submission_tracker.cpp", tracker)]
    },
}
(root / "release_paths.json").write_text(json.dumps(report, indent=2) + "\n")
