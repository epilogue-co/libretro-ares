#include <n64/n64.hpp>

namespace ares::Nintendo64 {

Vulkan vulkan;

struct LoggingInterface : Util::LoggingInterface {
  auto log(const char* tag, const char* fmt, va_list va) -> bool {
    char buffer[8192];
    vsnprintf(buffer, sizeof(buffer), fmt, va);
  //print(terminal::color::yellow(tag), buffer);
    return true;
  }
} loggingInterface;

// HW_RENDER Context: created in createDeviceFromInstance, destroyed in
// destroyExternalDevice. Deliberately a raw pointer rather than a
// std::unique_ptr so that no static destructor runs at process exit — at
// that point the frontend's VkDevice is already gone, and paraLLEl-RDP's
// ~Context calls vkDeviceWaitIdle unconditionally (it gates only the
// destroy on owned_device, not the wait). If the frontend forgets to
// invoke destroy_device we leak the Context, which is harmless at exit.
static ::Vulkan::Context* g_externalContext = nullptr;

struct Vulkan::Implementation {
  Implementation(u8* data, u32 size);
  ~Implementation();

  // Internal-mode Context. In external mode (HW_RENDER) it stays default-
  // constructed; device.set_context() points at g_externalContext instead.
  ::Vulkan::Context context;
  ::Vulkan::Device device;
  ::RDP::CommandProcessor* processor = nullptr;
  atomic<const char*> crash_error = nullptr;

  // HW_RENDER path keeps the latest scanout ImageHandle alive across the
  // frame so the VkImage/View remain valid until the frontend has consumed
  // them via set_image().
  ::Vulkan::ImageHandle hwRenderScanoutImage;

  struct Validation : public ::RDP::ValidationInterface {
    Implementation& self;
    Validation(Implementation& i) : self(i) {}
    void report_rdp_crash(::RDP::ValidationError err, const char *msg) override {
      self.crash_error = msg;
    }
  } validator{*this};

  //commands are u64 words, but the backend uses u32 swapped words.
  //size and offset are in u64 words.
  u32 buffer[0x10000] = {};
  u32 queueSize = 0;
  u32 queueOffset = 0;

  ::RDP::VIScanoutBuffer scanout;
  std::mutex lock;
  std::condition_variable condition;
  u32 scanoutCount = 0;
  u32 endCount = 0;
  uint64_t pendingTimeline = 0;
};

auto Vulkan::load(Node::Object) -> bool {
  if (vulkan.enable) {
    Util::set_thread_logging_interface(&loggingInterface);
    delete implementation;
    implementation = new Vulkan::Implementation(rdram.ram.data, rdram.ram.size);
    if(!implementation->processor) {
      delete implementation;
      implementation = nullptr;
    }

    if (!implementation) {
      platform->status("Vulkan init failed: No RDP rendering support");
      vulkan.enable = false;
    } else {
      platform->status("Vulkan Enabled: using paraLLEl-RDP");
    }
  } else {
    platform->status("Vulkan Disabled: No RDP rendering support");
  }

  return true;
}

auto Vulkan::unload() -> void {
  if (implementation) delete implementation;
  implementation = nullptr;
}

auto Vulkan::render() -> bool {
  if(!implementation) return false;

  static constexpr u32 commandLength[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };

  auto& command = rdp.command;

  u32 current = command.current & ~7;
  u32 end = command.end & ~7;
  u32 length = (end - current) / 8;
  if(current >= end) return true;

  u32* buffer = implementation->buffer;
  u32& queueSize = implementation->queueSize;
  u32& queueOffset = implementation->queueOffset;
  if(queueSize + length >= 0x8000) return true;

  if(!command.source) {
    do {
      buffer[queueSize * 2 + 0] = rdram.ram.read<Word>(current, RBusDevice::DP_DMA); current += 4;
      buffer[queueSize * 2 + 1] = rdram.ram.read<Word>(current, RBusDevice::DP_DMA); current += 4;
      queueSize++;
    } while(--length);
  } else {
    do {
      buffer[queueSize * 2 + 0] = rsp.dmem.read<Word>(current); current += 4;
      buffer[queueSize * 2 + 1] = rsp.dmem.read<Word>(current); current += 4;
      if(system.homebrewMode) {
        rsp.debugger.dmemReadWord(current - 8, 8, "RDP XBUS");
      }
      queueSize++;
    } while(--length);
  }

  while(queueOffset < queueSize) {
    u32 op = buffer[queueOffset * 2];
    u32 code = op >> 24 & 63;
    u32 length = commandLength[code];

    if(queueOffset + length > queueSize) {
      //partial command, keep data around for next processing call
      command.start = command.current = command.end;
      return true;
    }

    if(code >= 8) {
      implementation->processor->enqueue_command(length * 2, buffer + queueOffset * 2);
    }

    if(::RDP::Op(code) == ::RDP::Op::SyncFull) {
      //defer the wait: drain the previously-issued SyncFull before signalling a new one.
      //this pipelines GPU work one SyncFull behind the CPU so paraLLEl-RDP can render
      //the current frame in parallel with the next frame's CPU work. The wait still
      //happens (so DPI interrupt ordering is preserved relative to GPU completion),
      //just one frame later. Scanout enforces its own fence on the readback buffer.
      if(implementation->pendingTimeline) {
        implementation->processor->wait_for_timeline(implementation->pendingTimeline);
      }
      implementation->pendingTimeline = implementation->processor->signal_timeline();
      rdp.syncFull();
    }

    queueOffset += length;
  }

  queueOffset = 0;
  queueSize = 0;
  command.current = command.end;
  return true;
}

auto Vulkan::frame() -> void {
  if(!implementation) return;
  implementation->processor->begin_frame_context();
}

auto Vulkan::writeWord(u32 address, u32 data) -> void {
  if(!implementation) return;
  implementation->processor->set_vi_register(::RDP::VIRegister(address), data);
}

auto Vulkan::scanoutAsync(bool field) -> bool {
  if(!implementation) return false;

  { //wait until we're done reading in thread before we clobber the readback buffer
    std::unique_lock<std::mutex> lock{implementation->lock};
    implementation->condition.wait(lock, [this]() {
      return implementation->scanoutCount == implementation->endCount;
    });
  }

  //drain any deferred SyncFull before issuing scanout so the framebuffer reflects
  //the most recently completed RDP frame, not one still rendering.
  if(implementation->pendingTimeline) {
    implementation->processor->wait_for_timeline(implementation->pendingTimeline);
    implementation->pendingTimeline = 0;
  }

  implementation->processor->set_vi_register(::RDP::VIRegister::VCurrentLine, field);

  //0 steps if scanning out at upscaled resolution.
  //each downscale step reduces output resolution to [width, height] * max(1, upscale >> downscale_steps)
  ::RDP::ScanoutOptions options;
  options.downscale_steps = supersampleScanout ? 16 : 0;
  options.persist_frame_on_invalid_input = true;  //this is a compatibility hack, but I'm not sure what for ...
  if(disableVideoInterfaceProcessing) {
    options.vi = {false, false, true, false, false, false};
  }
  if(!supersampleScanout){
    options.blend_previous_frame = weaveDeinterlacing;
    options.upscale_deinterlacing = !weaveDeinterlacing;
  }
  else {
    options.blend_previous_frame = false;
    options.upscale_deinterlacing = true;
  }
  if(framePersistence) options.blend_previous_frame = true;


  if(hwRenderActive) {
    // Zero-copy GPU path: keep the rendered VkImage and publish it for the
    // libretro frontend to consume via set_image(). No CPU readback, no
    // host stall: scanout() drains the command-processing thread, signals
    // the renderer (flush_and_signal), and submits the VI command buffer
    // (containing the final image_barrier transitioning the returned image
    // to SHADER_READ_ONLY_OPTIMAL) to the graphics queue before returning.
    // The frontend submits its sampling work to the same queue, so in-queue
    // ordering provides the read-after-write guarantee — no semaphore or
    // host wait is required. Pattern matches mupen64plus-video-paraLLEl and
    // beetle-psx-hw, both of which omit any wait between scanout and
    // set_image.
    implementation->hwRenderScanoutImage =
      implementation->processor->scanout(options);
    if(implementation->hwRenderScanoutImage) {
      lastImage       = implementation->hwRenderScanoutImage->get_image();
      lastImageView   = implementation->hwRenderScanoutImage->get_view().get_view();
      lastImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      lastImageFormat = implementation->hwRenderScanoutImage->get_format();
      lastImageWidth  = implementation->hwRenderScanoutImage->get_width();
      lastImageHeight = implementation->hwRenderScanoutImage->get_height();
    } else {
      lastImage = VK_NULL_HANDLE;
      lastImageView = VK_NULL_HANDLE;
      lastImageWidth = lastImageHeight = 0;
    }
    implementation->scanoutCount++;
    return true;
  }

  if(implementation->scanout.fence) {
    implementation->scanout.fence->wait();
  }
  implementation->processor->scanout_async_buffer(implementation->scanout, options);
  implementation->scanoutCount++;
  return true;
}

auto Vulkan::mapScanoutRead(const u8*& rgba, u32& width, u32& height) -> void {
  if(!implementation || !implementation->scanout.fence || !implementation->scanout.width || !implementation->scanout.height) {
    rgba = nullptr;
    width = 0;
    height = 0;
  } else {
    implementation->scanout.fence->wait();
    rgba = (const u8*)implementation->device.map_host_buffer(*implementation->scanout.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
    width = implementation->scanout.width;
    height = implementation->scanout.height;
  }
}

auto Vulkan::unmapScanoutRead() -> void {
  if(implementation && implementation->scanout.buffer) {
    implementation->device.unmap_host_buffer(*implementation->scanout.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
  }
}

auto Vulkan::endScanout() -> void {
  if(implementation) {
    //notify main thread that we're done reading
    std::lock_guard<std::mutex> lock{implementation->lock};
    implementation->endCount++;
    implementation->condition.notify_one();
  }
}

auto Vulkan::crashed() -> const char* {
  if(implementation) return implementation->crash_error;
  return nullptr;
}

Vulkan::Implementation::Implementation(u8* data, u32 size) {
  // Two code paths:
  //  - External (HW_RENDER): g_externalContext was set up during libretro
  //    negotiation. Reuse it — DO NOT create a second device.
  //  - Internal (fallback): create our own instance + device as before.
  // If the caller flagged useExternalContext but g_externalContext was
  // never populated (or torn down), fail explicitly rather than silently
  // initialize an internal device on top — the frontend believes HW_RENDER
  // is live and our images on the internal device would be invisible to it.
  ::Vulkan::Context* activeContext = nullptr;
  if(vulkan.useExternalContext) {
    if(!g_externalContext) {
      crash_error = "HW_RENDER requested but external Vulkan context is missing";
      return;
    }
    activeContext = g_externalContext;
  } else {
    if(!::Vulkan::Context::init_loader(nullptr)) return;
    if(!context.init_instance_and_device(nullptr, 0, nullptr, 0, 0)) return;
    activeContext = &context;
  }
  device.set_context(*activeContext);
  device.init_frame_contexts(3);

  if(!vulkan.pipelineCache.empty()) {
    device.init_pipeline_cache(vulkan.pipelineCache.data(), vulkan.pipelineCache.size());
  }

  ::RDP::CommandProcessorFlags flags = 0;
  switch(vulkan.internalUpscale) {
  case 2: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT; break;
  case 4: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT; break;
  case 8: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT; break;
  }

  if(vulkan.internalUpscale > 1) {
    flags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
    //rasky: this is explicitly disabled because we want to make sure we don't
    // read back the super sampled version, as it can cause artifacts. We want
    // parallelRDP to also produce a 1x render to use for readbacks.
    //flags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
  }

  processor = new ::RDP::CommandProcessor(device, data, 0, size, size / 2, flags);
  if(!processor->device_is_supported()) {
    delete processor;
    processor = nullptr;
    return;
  }

  processor->set_validation_interface(&validator);
}

Vulkan::Implementation::~Implementation() {
  size_t cacheSize = device.get_pipeline_cache_size();
  if(cacheSize > 0) {
    vulkan.pipelineCache.resize(cacheSize);
    if(!device.get_pipeline_cache_data(vulkan.pipelineCache.data(), cacheSize)) {
      vulkan.pipelineCache.clear();
    }
  }
  if(processor) delete processor;
}

// === libretro Vulkan HW_RENDER negotiation ============================
// Implemented as bare-pointer helpers (opaque outContext) so we don't
// have to pull libretro_vulkan.h into ares core headers.

auto Vulkan::createDeviceFromInstance(
    void* outContext,
    VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr proc_addr,
    const char** required_device_extensions, unsigned num_required_device_extensions,
    const VkPhysicalDeviceFeatures* required_features) -> bool {
  if(!::Vulkan::Context::init_loader(proc_addr)) return false;

  // Per the libretro Vulkan v1/v2 negotiation contract, the frontend pairs
  // every create_device with a destroy_device. A second create_device
  // without an intervening destroy_device implies the frontend believes
  // the prior context is dead — but our Context wraps a still-live frontend
  // VkDevice. Refuse the duplicate rather than `delete g_externalContext`,
  // whose ~Context would call vkDeviceWaitIdle on the device the frontend
  // is still actively using.
  if(g_externalContext) return false;
  g_externalContext = new ::Vulkan::Context;
  if(!g_externalContext->init_device_from_instance(
       instance, gpu, surface,
       required_device_extensions, num_required_device_extensions,
       required_features, 0)) {
    delete g_externalContext;
    g_externalContext = nullptr;
    return false;
  }

  useExternalContext       = true;
  externalInstance         = g_externalContext->get_instance();
  externalGpu              = g_externalContext->get_gpu();
  externalDevice           = g_externalContext->get_device();
  externalQueue            = g_externalContext->get_queue_info().queues[::Vulkan::QUEUE_INDEX_GRAPHICS];
  externalQueueFamily      = g_externalContext->get_queue_info().family_indices[::Vulkan::QUEUE_INDEX_GRAPHICS];
  externalProcAddr         = proc_addr;

  // Per libretro_vulkan.h contract: the frontend owns BOTH the instance
  // (it created it before negotiation) AND the device (the spec at
  // retro_vulkan_destroy_device_t says destroy_device is called BEFORE
  // vkDestroyDevice, meaning the frontend does the final destroy). Release
  // ownership in paraLLEl-RDP's Context so its destructor will NOT call
  // vkDestroyDevice. Matches beetle-psx-hw and parallel-n64.
  g_externalContext->release_instance();
  g_externalContext->release_device();

  // Populate retro_vulkan_context (libretro_vulkan.h layout).
  struct { VkPhysicalDevice gpu; VkDevice device; VkQueue queue; uint32_t qfi;
           VkQueue pqueue; uint32_t pqfi; }* ctx = (decltype(ctx))outContext;
  ctx->gpu     = externalGpu;
  ctx->device  = externalDevice;
  ctx->queue   = externalQueue;
  ctx->qfi     = externalQueueFamily;
  ctx->pqueue  = externalQueue;
  ctx->pqfi    = externalQueueFamily;

  return true;
}

auto Vulkan::destroyExternalDevice() -> void {
  // libretro contract: this callback fires while the frontend's VkDevice is
  // still alive (frontend calls vkDestroyDevice AFTER us). Tear down all
  // device-bound core state in reverse construction order so paraLLEl-RDP's
  // Device/CommandProcessor/frame contexts release their resources against
  // a live device. The Context destructor will then run with release_device
  // already called (see createDeviceFromInstance) so it WON'T call
  // vkDestroyDevice — the frontend will.
  if(implementation) {
    delete implementation;
    implementation = nullptr;
  }
  if(g_externalContext) {
    delete g_externalContext;
    g_externalContext = nullptr;
  }
  useExternalContext       = false;
  externalInstance         = VK_NULL_HANDLE;
  externalGpu              = VK_NULL_HANDLE;
  externalDevice           = VK_NULL_HANDLE;
  externalQueue            = VK_NULL_HANDLE;
  externalQueueFamily      = 0;
  externalProcAddr         = nullptr;
  hwRenderActive           = false;
}

auto Vulkan::setHwRenderInterface(const void* iface) -> void {
  // The interface pointer is consumed by the libretro wrapper; we just
  // track whether HW_RENDER is live so scanoutAsync knows which path to
  // take.
  hwRenderActive = (iface != nullptr);
}

}
