#include "rdp_device.hpp"

#include <vector>

namespace ares::Nintendo64 {

struct Vulkan {
  auto load(Node::Object) -> bool;
  auto unload() -> void;

  auto render() -> bool;
  auto frame() -> void;
  auto writeWord(u32 address, u32 data) -> void;
  auto scanoutAsync(bool field) -> bool;
  auto mapScanoutRead(const u8*& rgba, u32& width, u32& height) -> void;
  auto unmapScanoutRead() -> void;
  auto endScanout() -> void;
  auto crashed() -> const char*;

  // === HW_RENDER (libretro Vulkan zero-copy handoff) ===================
  // When useExternalContext is true at load(), paraLLEl-RDP is initialized
  // against the externally-provided instance/device rather than creating
  // its own. scanoutAsync then publishes the rendered VkImage in the
  // lastImage* fields below instead of copying to a host buffer.
  bool useExternalContext = false;
  VkInstance       externalInstance     = VK_NULL_HANDLE;
  VkPhysicalDevice externalGpu          = VK_NULL_HANDLE;
  VkDevice         externalDevice       = VK_NULL_HANDLE;
  VkQueue          externalQueue        = VK_NULL_HANDLE;
  uint32_t         externalQueueFamily  = 0;
  PFN_vkGetInstanceProcAddr externalProcAddr = nullptr;

  // Per-frame handoff state (valid between scanoutAsync and endScanout).
  bool         hwRenderActive   = false;
  VkImage      lastImage        = VK_NULL_HANDLE;
  VkImageView  lastImageView    = VK_NULL_HANDLE;
  VkImageLayout lastImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat     lastImageFormat  = VK_FORMAT_UNDEFINED;
  uint32_t     lastImageWidth   = 0;
  uint32_t     lastImageHeight  = 0;

  // Zero-copy publish (paraLLEl-RDP export_scanout + frontend memory
  // import). Captured by scanoutAsync, consumed by the libretro wrapper.
  VkSemaphore lastSignalSemaphore  = VK_NULL_HANDLE;
  uint64_t    lastAllocationId     = 0;
  uintptr_t   lastMemoryHandle     = 0;
  uint64_t    lastMemorySize       = 0;
  uint32_t    lastMemoryHandleType = 0;
  bool        memoryPublishPending = false;
  // GL→Vk back-pressure semaphore: created by us, registered with the
  // frontend via set_signal_semaphore, signaled by GL after blit, waited
  // on by our next scanout submit. Closes the recycle-race window where
  // paraLLEl-RDP might overwrite a scanout buffer GL is still reading.
  VkSemaphore lastGlSignalVkSem    = VK_NULL_HANDLE;

  // libretro v1 negotiation helper. Picks queue family, creates the
  // device via paraLLEl-RDP's Context::init_device_from_instance, and
  // returns the bare Vulkan handles for the frontend to consume.
  auto createDeviceFromInstance(
    void* /*retro_vulkan_context* (opaque to avoid libretro coupling)*/ outContext,
    VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr proc_addr,
    const char** required_device_extensions, unsigned num_required_device_extensions,
    const VkPhysicalDeviceFeatures* required_features) -> bool;

  // libretro v1 destroy_device hook.
  auto destroyExternalDevice() -> void;

  // Set / clear the live HW interface handle. Opaque pointer to avoid
  // forcing libretro_vulkan.h into ares core headers.
  auto setHwRenderInterface(const void* iface) -> void;

  struct Implementation;
  Implementation* implementation = nullptr;

  bool enable = true;
  bool disableVideoInterfaceProcessing = false;
  bool weaveDeinterlacing = false;
  bool framePersistence = false;

  // Frame dedupe. When dedupeFrames is set, scanoutAsync fingerprints the
  // VI register state plus the scanned RDRAM framebuffer and, on a repeat
  // of the previous field, sets duplicateFrame and skips the GPU scanout
  // entirely so the libretro layer emits a NULL dupe. lastFrameKey holds
  // the previous fingerprint.
  bool dedupeFrames      = false;
  bool duplicateFrame    = false;
  bool lastFrameKeyValid = false;
  u64  lastFrameKey      = 0;
  // Running counts for the periodic dedupe log summary (reset each report).
  u64  dedupeSkipped     = 0;
  u64  dedupePresented   = 0;
  u32  dedupeReportTimer = 0;
  u32  internalUpscale = 1;  //1, 2, 4, 8
  bool supersampleScanout = false;
  u32  outputUpscale = supersampleScanout ? 1 : internalUpscale;

  std::vector<u8> pipelineCache;
};

extern Vulkan vulkan;

}
