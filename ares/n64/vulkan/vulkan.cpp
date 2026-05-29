#include <n64/n64.hpp>

#include <cstring>

namespace ares::Nintendo64 {

Vulkan vulkan;

// === Frame-dedupe fingerprint ==========================================
// FNV-1a style mix; not cryptographic, but a 64-bit collision over a
// multi-hour session is negligible and a miss only repeats one field.
static inline auto dedupeMix(u64 h, u64 value) -> u64 {
  h ^= value;
  h *= 0x100000001b3ull;
  return h;
}

static auto dedupeHashBytes(u64 h, const u8* data, size_t size) -> u64 {
  size_t i = 0;
  for(; i + 8 <= size; i += 8) {
    u64 chunk;
    memcpy(&chunk, data + i, sizeof(chunk));
    h = dedupeMix(h, chunk);
  }
  for(; i < size; i++) h = dedupeMix(h, data[i]);
  return h;
}

// Fingerprint the frame the VI is about to scan: the register state that
// shapes the displayed image, plus a content hash of the scanned RDRAM
// framebuffer. Two consecutive fields with the same fingerprint are
// pixel-identical on screen. Field parity is folded in so the two fields
// of an interlaced frame never dedupe against each other.
static auto dedupeFingerprint(bool field) -> u64 {
  // NOTE: dramAddress is deliberately NOT part of the fingerprint. It only
  // says *where* the framebuffer lives; the displayed image is decided by
  // the pixels (hashed below) plus how they're scaled/cropped/fielded. A
  // double-buffered game flips dramAddress between two buffers holding
  // identical pixels, so folding the address in would wrongly treat those
  // as distinct frames (a static screen would only dedupe ~50%).
  u64 h = 0xcbf29ce484222325ull;
  h = dedupeMix(h, vi.io.width);
  h = dedupeMix(h, vi.io.colorDepth);
  h = dedupeMix(h, (u64)vi.io.xscale << 12 | vi.io.xsubpixel);
  h = dedupeMix(h, (u64)vi.io.yscale << 12 | vi.io.ysubpixel);
  h = dedupeMix(h, (u64)vi.io.hstart << 10 | vi.io.hend);
  h = dedupeMix(h, (u64)vi.io.vstart << 10 | vi.io.vend);
  h = dedupeMix(h, (u64)vi.io.serrate << 1 | (u64)field);

  u32 bytesPerPixel = vi.io.colorDepth == 2 ? 2 : vi.io.colorDepth == 3 ? 4 : 0;
  if(bytesPerPixel) {
    u32 activeLines = Region::PAL() ? 576 : 480;
    u32 sourceRows  = (((u32)vi.io.ysubpixel + (u32)vi.io.yscale * activeLines) >> 11) + 1;
    u64 span = (u64)vi.io.width * bytesPerPixel * sourceRows;
    u32 base = vi.io.dramAddress;
    if(base < rdram.ram.size) {
      u64 available = (u64)rdram.ram.size - base;
      if(span > available) span = available;
      h = dedupeHashBytes(h, rdram.ram.data + base, (size_t)span);
    }
  }
  return h;
}

// Periodic summary so the dedupe can be observed without flooding the log
// with one line per skipped field. Emitted via platform->status (INFO) only
// while dedupe is enabled, roughly once every two seconds of output.
static auto dedupeReport() -> void {
  if(++vulkan.dedupeReportTimer < 120) return;
  u64 total = vulkan.dedupeSkipped + vulkan.dedupePresented;
  if(total && platform) {
    u32 percent = (u32)(vulkan.dedupeSkipped * 100 / total);
    string message{"ares: frame dedupe — skipped ", vulkan.dedupeSkipped,
                   "/", total, " fields (", percent, "%)"};
    platform->status(message);
  }
  vulkan.dedupeReportTimer = 0;
  vulkan.dedupeSkipped = 0;
  vulkan.dedupePresented = 0;
}

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

  // Per-scanout external binary semaphore (OPAQUE_FD / OPAQUE_WIN32). Held
  // here so the wrapped Granite handle survives until set_image is done.
  // hwRenderSignalSemPrev keeps the previous frame's handle alive until
  // the frontend's GL thread has had a chance to import it via
  // vkGetSemaphoreFdKHR — see RetroVulkanBridge::iface_set_image.
  ::Vulkan::Semaphore hwRenderSignalSem;
  ::Vulkan::Semaphore hwRenderSignalSemPrev;
  // GL→Vk back-pressure semaphore: created once, reused every frame in
  // binary-semaphore signal/wait cycle. Frontend signals; we wait.
  ::Vulkan::Semaphore hwRenderGlSignalSem;
  bool                hwRenderHasGlSignal = false;
  // Tracks the underlying VkDeviceMemory we last exported, so we only
  // re-publish when the underlying allocation rotates.
  VkDeviceMemory hwRenderLastVkMemory = VK_NULL_HANDLE;
  uint64_t       hwRenderAllocCounter = 0;

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
  duplicateFrame = false;
  lastFrameKeyValid = false;
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

  // Frame dedupe: fingerprint the now-settled frame; on a repeat, skip the
  // GPU scanout + semaphore handoff entirely and flag the frame so
  // Program::video emits a libretro NULL dupe. Disabled when the scanout
  // blends the previous frame (motion blur / weave), since a static source
  // still yields a changing image there.
  duplicateFrame = false;
  bool blendsPreviousFrame = framePersistence || (weaveDeinterlacing && !supersampleScanout);
  // hostCoherent gate: the fingerprint reads host RDRAM, which is only current on
  // the coherent import path. On the incoherent fallback it is stale at this point
  // (resolved only inside the later scanout(), which a dedupe skips), so deduping
  // there would latch into a permanent freeze — keep it off.
  if(dedupeFrames && hostCoherent && !blendsPreviousFrame) {
    u64 key = dedupeFingerprint(field);
    if(lastFrameKeyValid && key == lastFrameKey) {
      duplicateFrame = true;
      dedupeSkipped++;
      dedupeReport();
      // No scanout issued this field. Bump scanoutCount so it stays paired
      // with the endScanout() that VI::refresh() always calls, keeping the
      // readback wait at the top of the next scanoutAsync from deadlocking.
      // Issuing no scanout also means no new GL→Vk wait/signal is set up, so
      // the 1 real scanout ↔ 1 real blit semaphore pairing is preserved.
      implementation->scanoutCount++;
      return true;
    }
    lastFrameKey = key;
    lastFrameKeyValid = true;
    dedupePresented++;
    dedupeReport();
  } else {
    lastFrameKeyValid = false;
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
    // libretro frontend to consume via set_image(). On Linux/Windows we
    // additionally request paraLLEl-RDP to allocate the scanout image
    // with external memory + mint an exportable signal semaphore, so the
    // frontend can import the underlying VkDeviceMemory into GL directly
    // (no vkCmdCopyImage) and wait on the actual Vulkan completion.
    //
    // macOS deliberately skips both: MoltenVK doesn't expose OPAQUE_FD
    // image creation or semaphore export the way the Khronos KHR
    // extensions assume, so paraLLEl-RDP fails to allocate the scanout
    // image (returns nullptr) and the frame goes black. The frontend's
    // IOSurface bridge handles the macOS HW handoff via a vkCmdCopyImage
    // into a CGLTexImageIOSurface2D-backed VkImage instead.
    #if !defined(__APPLE__)
    options.export_scanout     = true;
    options.export_handle_type = ::Vulkan::ExternalHandle::get_opaque_memory_handle_type();
    options.persist_frame_on_invalid_input = false;  // incompatible with export_scanout
    implementation->hwRenderSignalSemPrev = implementation->hwRenderSignalSem;
    implementation->hwRenderSignalSem = implementation->device.request_semaphore_external(
      VK_SEMAPHORE_TYPE_BINARY_KHR,
      ::Vulkan::ExternalHandle::get_opaque_semaphore_handle_type());
    options.signal_semaphore   = implementation->hwRenderSignalSem;

    // GL→Vk back-pressure. Minted lazily; reused every frame.
    if(!implementation->hwRenderGlSignalSem) {
      implementation->hwRenderGlSignalSem = implementation->device.request_semaphore_external(
        VK_SEMAPHORE_TYPE_BINARY_KHR,
        ::Vulkan::ExternalHandle::get_opaque_semaphore_handle_type());
      if(implementation->hwRenderGlSignalSem) {
        lastGlSignalVkSem = implementation->hwRenderGlSignalSem->get_semaphore();
      }
    }
    if(implementation->hwRenderHasGlSignal && implementation->hwRenderGlSignalSem) {
      // Skip wait on the very first scanout — no GL signal yet to wait on.
      options.wait_semaphore = implementation->hwRenderGlSignalSem;
    }
    #endif

    implementation->hwRenderScanoutImage =
      implementation->processor->scanout(options);
    if(implementation->hwRenderScanoutImage) {
      lastImage       = implementation->hwRenderScanoutImage->get_image();
      lastImageView   = implementation->hwRenderScanoutImage->get_view().get_view();
      lastImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      lastImageFormat = implementation->hwRenderScanoutImage->get_format();
      lastImageWidth  = implementation->hwRenderScanoutImage->get_width();
      lastImageHeight = implementation->hwRenderScanoutImage->get_height();

      #if !defined(__APPLE__)
      lastSignalSemaphore = implementation->hwRenderSignalSem
        ? implementation->hwRenderSignalSem->get_semaphore()
        : VK_NULL_HANDLE;

      // Successful scanout — from next frame, wait on the GL signal sem.
      // (Program::video calls set_signal_semaphore with lastGlSignalVkSem
      // so the frontend signals it after the blit of THIS frame.)
      if(implementation->hwRenderGlSignalSem) implementation->hwRenderHasGlSignal = true;

      // Detect a fresh underlying VkDeviceMemory (paraLLEl-RDP recycles
      // scanout images, so the same backing memory typically returns).
      // Only re-export when it changes.
      auto& alloc = const_cast<::Vulkan::DeviceAllocation&>(
        implementation->hwRenderScanoutImage->get_allocation());
      VkDeviceMemory mem = alloc.get_memory();
      if(mem != VK_NULL_HANDLE && mem != implementation->hwRenderLastVkMemory) {
        auto handle = alloc.export_handle(implementation->device);
        if((bool)handle) {
          implementation->hwRenderLastVkMemory = mem;
          implementation->hwRenderAllocCounter++;
          lastAllocationId = implementation->hwRenderAllocCounter;
          #ifdef _WIN32
          lastMemoryHandle = reinterpret_cast<uintptr_t>(handle.handle);
          #else
          lastMemoryHandle = static_cast<uintptr_t>(static_cast<unsigned>(handle.handle));
          #endif
          lastMemorySize       = static_cast<uint64_t>(alloc.get_size());
          lastMemoryHandleType = static_cast<uint32_t>(handle.memory_handle_type);
          memoryPublishPending = true;
        }
      }
      #else
      lastSignalSemaphore = VK_NULL_HANDLE;
      #endif
    } else {
      lastImage = VK_NULL_HANDLE;
      lastImageView = VK_NULL_HANDLE;
      lastImageWidth = lastImageHeight = 0;
      lastSignalSemaphore = VK_NULL_HANDLE;
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

  // Frame dedupe fingerprints host RDRAM; that is only valid when RDRAM is
  // imported coherently. On the incoherent fallback (no VK_EXT_external_memory_host)
  // the hash reads stale bytes and dedupe would latch into a freeze, so record
  // coherency here and gate dedupe on it in scanoutAsync.
  vulkan.hostCoherent = processor->is_host_memory_coherent();
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
