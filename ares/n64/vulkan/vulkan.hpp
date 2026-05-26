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
  u32  internalUpscale = 1;  //1, 2, 4, 8
  bool supersampleScanout = false;
  u32  outputUpscale = supersampleScanout ? 1 : internalUpscale;

  std::vector<u8> pipelineCache;
};

extern Vulkan vulkan;

}
