template<u32 Size>
inline auto Bus::read(u32 address, Thread& thread, RBusDevice device) -> u64 {
  static_assert(Size == Byte || Size == Half || Size == Word || Size == Dual);

  if(address <= 0x03ef'ffff) return rdram.ram.read<Size>(address, device);
  if(address <= 0x03ff'ffff) return rdram.read<Size>(address, thread);
  if(Size == Dual)           return freezeDualRead(address), 0;
  if(address <= 0x04ff'ffff) {
    switch((address >> 20) & 0xf) {
    case 0x0:
      if(address <= 0x0407'ffff) return rsp.read<Size>(address, thread);
      if(address <= 0x040b'ffff) return rsp.status.read<Size>(address, thread);
      return freezeUnmapped(address), 0;
    case 0x1: return rdp.read<Size>(address, thread);
    case 0x2: return rdp.io.read<Size>(address, thread);
    case 0x3: return mi.read<Size>(address, thread);
    case 0x4: return vi.read<Size>(address, thread);
    case 0x5: return ai.read<Size>(address, thread);
    case 0x6: return pi.read<Size>(address, thread);
    case 0x7: return ri.read<Size>(address, thread);
    case 0x8: return si.read<Size>(address, thread);
    default:  return freezeUnmapped(address), 0;
    }
  }
  if(address <= 0x1fbf'ffff) return pi.read<Size>(address, thread);
  if(address <= 0x1fcf'ffff) return si.read<Size>(address, thread);
  if(address <= 0x7fff'ffff) return pi.read<Size>(address, thread);
  if(Model::Aleck64())       return aleck64.read<Size>(address, thread);
  return freezeUnmapped(address), 0;
}

template<u32 Size>
inline auto Bus::readBurst(u32 address, u32 *data, Thread& thread) -> bool {
  static_assert(Size == DCache || Size == ICache);
  RBusDevice device;
  if constexpr(Size == DCache) device = RBusDevice::VR4300_DCACHE;
  if constexpr(Size == ICache) device = RBusDevice::VR4300_ICACHE;

  if(address <= 0x03ef'ffff) return rdram.ram.readBurst<Size>(address, data, device), true;
  if(address <= 0x03ff'ffff) {
    // FIXME: not hardware validated, no idea of the behavior
    data[0] = rdram.readWord(address | 0x0, thread);
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;
    if constexpr(Size == ICache) {
      data[4] = 0;
      data[5] = 0;
      data[6] = 0;
      data[7] = 0;
    }
    return true;
  }

  if(Model::Aleck64()) {
    if(address <= 0xbfff'ffff) return freezeUncached(address), false;
    if(address <= 0xc07f'ffff) return aleck64.sdram.readBurst<Size>(address, data, device), true;
  }

  return freezeUncached(address), false;
}

template<u32 Size>
inline auto Bus::write(u32 address, u64 data, Thread& thread, RBusDevice device) -> void {
  static_assert(Size == Byte || Size == Half || Size == Word || Size == Dual);
  if constexpr(Accuracy::CPU::Recompiler) {
    cpu.recompiler.invalidateRange(address, Size);
  }

  if(address <= 0x03ef'ffff) return rdram.ram.write<Size>(address, data, device);
  if(address <= 0x03ff'ffff) return rdram.write<Size>(address, data, thread);
  if(address <= 0x04ff'ffff) {
    switch((address >> 20) & 0xf) {
    case 0x0:
      if(address <= 0x0407'ffff) return rsp.write<Size>(address, data, thread);
      if(address <= 0x040b'ffff) return rsp.status.write<Size>(address, data, thread);
      return freezeUnmapped(address);
    case 0x1: return rdp.write<Size>(address, data, thread);
    case 0x2: return rdp.io.write<Size>(address, data, thread);
    case 0x3: return mi.write<Size>(address, data, thread);
    case 0x4: return vi.write<Size>(address, data, thread);
    case 0x5: return ai.write<Size>(address, data, thread);
    case 0x6: return pi.write<Size>(address, data, thread);
    case 0x7: return ri.write<Size>(address, data, thread);
    case 0x8: return si.write<Size>(address, data, thread);
    default:  return freezeUnmapped(address);
    }
  }
  if(address <= 0x1fbf'ffff) return pi.write<Size>(address, data, thread);
  if(address <= 0x1fcf'ffff) return si.write<Size>(address, data, thread);
  if(address <= 0x7fff'ffff) return pi.write<Size>(address, data, thread);
  if(Model::Aleck64())       return aleck64.write<Size>(address, data, thread);
  return freezeUnmapped(address);
}

template<u32 Size>
inline auto Bus::writeBurst(u32 address, u32 *data, Thread& thread) -> bool {
  static_assert(Size == DCache || Size == ICache);
  RBusDevice device;
  if constexpr(Size == DCache) device = RBusDevice::VR4300_DCACHE;
  if constexpr(Size == ICache) device = RBusDevice::VR4300_ICACHE;

  if constexpr(Accuracy::CPU::Recompiler) {
    cpu.recompiler.invalidateRange(address, Size == DCache ? 16 : 32);
  }

  if(address <= 0x03ef'ffff) return rdram.ram.writeBurst<Size>(address, data, device), true;
  if(address <= 0x03ff'ffff) {
    // FIXME: not hardware validated, but a good guess
    rdram.writeWord(address | 0x0, data[0], thread);
    return true;
  }

  if(Model::Aleck64()) {
    if(address <= 0xbfff'ffff) return freezeUncached(address), false;
    if(address <= 0xc07f'ffff) return aleck64.sdram.writeBurst<Size>(address, data, device), true;
  }

  return freezeUncached(address), false;
}

inline auto Bus::freezeUnmapped(u32 address) -> void {
  debug(unusual, "[Bus::freezeUnmapped] CPU frozen because of access to RCP unmapped area: 0x", hex(address, 8L), " (PC: ", hex(cpu.ipu.pc, 8L), ")");
  if(system.homebrewMode && cpu.emuxState.excMask.bit(3)) {
    cpu.emuxException(3);
    cpu.exception.emux();
    return;
  }
  cpu.scc.sysadFrozen = true;
}

inline auto Bus::freezeUncached(u32 address) -> void {
  debug(unusual, "[Bus::freezeUncached] CPU frozen because of cached access to non-RDRAM area: 0x", hex(address, 8L), " (PC: ", hex(cpu.ipu.pc, 8L), ")");
  if(system.homebrewMode && cpu.emuxState.excMask.bit(1)) {
    cpu.emuxException(1);
    cpu.exception.emux();
    return;
  }
  cpu.scc.sysadFrozen = true;
}

inline auto Bus::freezeDualRead(u32 address) -> void {
  debug(unusual, "[Bus::freezeDualRead] CPU frozen because of 64-bit read from non-RDRAM area: 0x ", hex(address, 8L), " (PC: ", hex(cpu.ipu.pc, 8L), ")");
  if(system.homebrewMode && cpu.emuxState.excMask.bit(2)) {
    cpu.emuxException(2);
    cpu.exception.emux();
    return;
  }
  cpu.scc.sysadFrozen = true;
}
