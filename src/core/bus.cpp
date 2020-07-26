#include "bus.h"
#include "cdrom.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "dma.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
Log_SetChannel(Bus);

std::unique_ptr<Bus> g_bus;

#define FIXUP_WORD_READ_OFFSET(offset) ((offset) & ~u32(3))
#define FIXUP_WORD_READ_VALUE(offset, value) ((value) >> (((offset)&u32(3)) * 8u))
#define FIXUP_HALFWORD_READ_OFFSET(offset) ((offset) & ~u32(1))
#define FIXUP_HALFWORD_READ_VALUE(offset, value) ((value) >> (((offset)&u32(1)) * 8u))
#define FIXUP_HALFWORD_WRITE_VALUE(offset, value) ((value) << (((offset)&u32(1)) * 8u))

// Offset and value remapping for (w32) registers from nocash docs.
ALWAYS_INLINE static void FixupUnalignedWordAccessW32(u32& offset, u32& value)
{
  const u32 byte_offset = offset & u32(3);
  offset &= ~u32(3);
  value <<= byte_offset * 8;
}

Bus::Bus() = default;

Bus::~Bus() = default;

void Bus::Initialize()
{
  //
}

void Bus::Reset()
{
  std::memset(m_ram, 0, sizeof(m_ram));
  m_MEMCTRL.exp1_base = 0x1F000000;
  m_MEMCTRL.exp2_base = 0x1F802000;
  m_MEMCTRL.exp1_delay_size.bits = 0x0013243F;
  m_MEMCTRL.exp3_delay_size.bits = 0x00003022;
  m_MEMCTRL.bios_delay_size.bits = 0x0013243F;
  m_MEMCTRL.spu_delay_size.bits = 0x200931E1;
  m_MEMCTRL.cdrom_delay_size.bits = 0x00020843;
  m_MEMCTRL.exp2_delay_size.bits = 0x00070777;
  m_MEMCTRL.common_delay.bits = 0x00031125;
  m_ram_size_reg = UINT32_C(0x00000B88);
  RecalculateMemoryTimings();
}

bool Bus::DoState(StateWrapper& sw)
{
  sw.Do(&m_exp1_access_time);
  sw.Do(&m_exp2_access_time);
  sw.Do(&m_bios_access_time);
  sw.Do(&m_cdrom_access_time);
  sw.Do(&m_spu_access_time);
  sw.DoBytes(m_ram, sizeof(m_ram));
  sw.DoBytes(m_bios, sizeof(m_bios));
  sw.DoArray(m_MEMCTRL.regs, countof(m_MEMCTRL.regs));
  sw.Do(&m_ram_size_reg);
  sw.Do(&m_tty_line_buffer);
  return !sw.HasError();
}

bool Bus::ReadByte(PhysicalMemoryAddress address, u8* value)
{
  u32 temp = 0;
  const bool result = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  *value = Truncate8(temp);
  return result;
}

bool Bus::ReadHalfWord(PhysicalMemoryAddress address, u16* value)
{
  u32 temp = 0;
  const bool result = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  *value = Truncate16(temp);
  return result;
}

bool Bus::ReadWord(PhysicalMemoryAddress address, u32* value)
{
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, *value);
}

bool Bus::WriteByte(PhysicalMemoryAddress address, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
}

bool Bus::WriteHalfWord(PhysicalMemoryAddress address, u16 value)
{
  u32 temp = ZeroExtend32(value);
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
}

bool Bus::WriteWord(PhysicalMemoryAddress address, u32 value)
{
  return DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
}

TickCount Bus::ReadWords(PhysicalMemoryAddress address, u32* words, u32 word_count)
{
  if (address + (word_count * sizeof(u32)) > (RAM_BASE + RAM_SIZE))
  {
    // Not RAM, or RAM mirrors.
    TickCount total_ticks = 0;
    for (u32 i = 0; i < word_count; i++)
    {
      const TickCount ticks = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, words[i]);
      if (ticks < 0)
        return -1;

      total_ticks += ticks;
      address += sizeof(u32);
    }

    return total_ticks;
  }

  std::memcpy(words, &m_ram[address], sizeof(u32) * word_count);
  return GetDMARAMTickCount(word_count);
}

TickCount Bus::WriteWords(PhysicalMemoryAddress address, const u32* words, u32 word_count)
{
  if (address + (word_count * sizeof(u32)) > (RAM_BASE + RAM_SIZE))
  {
    // Not RAM, or RAM mirrors.
    TickCount total_ticks = 0;
    for (u32 i = 0; i < word_count; i++)
    {
      u32 value = words[i];
      const TickCount ticks = DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
      if (ticks < 0)
        return -1;

      total_ticks += ticks;
      address += sizeof(u32);
    }

    return total_ticks;
  }

  std::memcpy(&m_ram[address], words, sizeof(u32) * word_count);
  InvalidateCodePages(address, word_count);
  return GetDMARAMTickCount(word_count);
}

void Bus::SetExpansionROM(std::vector<u8> data) { m_exp1_rom = std::move(data); }

void Bus::SetBIOS(const std::vector<u8>& image)
{
  if (image.size() != static_cast<u32>(BIOS_SIZE))
  {
    Panic("Incorrect BIOS image size");
    return;
  }

  std::memcpy(m_bios, image.data(), BIOS_SIZE);
}

std::tuple<TickCount, TickCount, TickCount> Bus::CalculateMemoryTiming(MEMDELAY mem_delay, COMDELAY common_delay)
{
  // from nocash spec
  s32 first = 0, seq = 0, min = 0;
  if (mem_delay.use_com0_time)
  {
    first += s32(common_delay.com0) - 1;
    seq += s32(common_delay.com0) - 1;
  }
  if (mem_delay.use_com2_time)
  {
    first += s32(common_delay.com2);
    seq += s32(common_delay.com2);
  }
  if (mem_delay.use_com3_time)
  {
    min = s32(common_delay.com3);
  }
  if (first < 6)
    first++;

  first = first + s32(mem_delay.access_time) + 2;
  seq = seq + s32(mem_delay.access_time) + 2;

  if (first < (min + 6))
    first = min + 6;
  if (seq < (min + 2))
    seq = min + 2;

  const TickCount byte_access_time = first;
  const TickCount halfword_access_time = mem_delay.data_bus_16bit ? first : (first + seq);
  const TickCount word_access_time = mem_delay.data_bus_16bit ? (first + seq) : (first + seq + seq + seq);
  return std::tie(std::max(byte_access_time - 1, 0), std::max(halfword_access_time - 1, 0),
                  std::max(word_access_time - 1, 0));
}

void Bus::RecalculateMemoryTimings()
{
  std::tie(m_bios_access_time[0], m_bios_access_time[1], m_bios_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.bios_delay_size, m_MEMCTRL.common_delay);
  std::tie(m_cdrom_access_time[0], m_cdrom_access_time[1], m_cdrom_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.cdrom_delay_size, m_MEMCTRL.common_delay);
  std::tie(m_spu_access_time[0], m_spu_access_time[1], m_spu_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.spu_delay_size, m_MEMCTRL.common_delay);

  Log_TracePrintf("BIOS Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.bios_delay_size.data_bus_16bit ? 16 : 8, m_bios_access_time[0] + 1,
                  m_bios_access_time[1] + 1, m_bios_access_time[2] + 1);
  Log_TracePrintf("CDROM Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.cdrom_delay_size.data_bus_16bit ? 16 : 8, m_cdrom_access_time[0] + 1,
                  m_cdrom_access_time[1] + 1, m_cdrom_access_time[2] + 1);
  Log_TracePrintf("SPU Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.spu_delay_size.data_bus_16bit ? 16 : 8, m_spu_access_time[0] + 1, m_spu_access_time[1] + 1,
                  m_spu_access_time[2] + 1);
}

TickCount Bus::DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress address, u32& value)
{
  SmallString str;
  str.AppendString("Invalid bus ");
  if (size == MemoryAccessSize::Byte)
    str.AppendString("byte");
  if (size == MemoryAccessSize::HalfWord)
    str.AppendString("word");
  if (size == MemoryAccessSize::Word)
    str.AppendString("dword");
  str.AppendCharacter(' ');
  if (type == MemoryAccessType::Read)
    str.AppendString("read");
  else
    str.AppendString("write");

  str.AppendFormattedString(" at address 0x%08X", address);
  if (type == MemoryAccessType::Write)
    str.AppendFormattedString(" (value 0x%08X)", value);

  Log_ErrorPrint(str);
  if (type == MemoryAccessType::Read)
    value = UINT32_C(0xFFFFFFFF);

  return 1;
}

u32 Bus::DoReadEXP1(MemoryAccessSize size, u32 offset)
{
  if (m_exp1_rom.empty())
  {
    // EXP1 not present.
    return UINT32_C(0xFFFFFFFF);
  }

  if (offset == 0x20018)
  {
    // Bit 0 - Action Replay On/Off
    return UINT32_C(1);
  }

  const u32 transfer_size = u32(1) << static_cast<u32>(size);
  if ((offset + transfer_size) > m_exp1_rom.size())
  {
    return UINT32_C(0);
  }

  u32 value;
  if (size == MemoryAccessSize::Byte)
  {
    value = ZeroExtend32(m_exp1_rom[offset]);
  }
  else if (size == MemoryAccessSize::HalfWord)
  {
    u16 halfword;
    std::memcpy(&halfword, &m_exp1_rom[offset], sizeof(halfword));
    value = ZeroExtend32(halfword);
  }
  else
  {
    std::memcpy(&value, &m_exp1_rom[offset], sizeof(value));
  }

  // Log_DevPrintf("EXP1 read: 0x%08X -> 0x%08X", EXP1_BASE | offset, value);
  return value;
}

void Bus::DoWriteEXP1(MemoryAccessSize size, u32 offset, u32 value)
{
  Log_WarningPrintf("EXP1 write: 0x%08X <- 0x%08X", EXP1_BASE | offset, value);
}

u32 Bus::DoReadEXP2(MemoryAccessSize size, u32 offset)
{
  // rx/tx buffer empty
  if (offset == 0x21)
  {
    return 0x04 | 0x08;
  }

  Log_WarningPrintf("EXP2 read: 0x%08X", EXP2_BASE | offset);
  return UINT32_C(0xFFFFFFFF);
}

void Bus::DoWriteEXP2(MemoryAccessSize size, u32 offset, u32 value)
{
  if (offset == 0x23)
  {
    if (value == '\r')
      return;

    if (value == '\n')
    {
      if (!m_tty_line_buffer.empty())
      {
        Log_InfoPrintf("TTY: %s", m_tty_line_buffer.c_str());
#ifdef _DEBUG
        if (CPU::LOG_EXECUTION)
          CPU::WriteToExecutionLog("TTY: %s\n", m_tty_line_buffer.c_str());
#endif
      }
      m_tty_line_buffer.clear();
    }
    else
    {
      m_tty_line_buffer += static_cast<char>(Truncate8(value));
    }

    return;
  }

  if (offset == 0x41)
  {
    Log_WarningPrintf("BIOS POST status: %02X", value & UINT32_C(0x0F));
    return;
  }

  Log_WarningPrintf("EXP2 write: 0x%08X <- 0x%08X", EXP2_BASE | offset, value);
}

u32 Bus::DoReadMemoryControl(MemoryAccessSize size, u32 offset)
{
  u32 value = m_MEMCTRL.regs[offset / 4];
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteMemoryControl(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);

  const u32 index = offset / 4;
  const u32 write_mask = (index == 8) ? COMDELAY::WRITE_MASK : MEMDELAY::WRITE_MASK;
  const u32 new_value = (m_MEMCTRL.regs[index] & ~write_mask) | (value & write_mask);
  if (m_MEMCTRL.regs[index] != new_value)
  {
    m_MEMCTRL.regs[index] = new_value;
    RecalculateMemoryTimings();
  }
}

u32 Bus::DoReadMemoryControl2(MemoryAccessSize size, u32 offset)
{
  if (offset == 0x00)
    return m_ram_size_reg;

  u32 value = 0;
  DoInvalidAccess(MemoryAccessType::Read, size, MEMCTRL2_BASE | offset, value);
  return value;
}

void Bus::DoWriteMemoryControl2(MemoryAccessSize size, u32 offset, u32 value)
{
  if (offset == 0x00)
  {
    m_ram_size_reg = value;
    return;
  }

  DoInvalidAccess(MemoryAccessType::Write, size, MEMCTRL2_BASE | offset, value);
}

u32 Bus::DoReadPad(MemoryAccessSize size, u32 offset) { return g_pad.ReadRegister(offset); }

void Bus::DoWritePad(MemoryAccessSize size, u32 offset, u32 value) { g_pad.WriteRegister(offset, value); }

u32 Bus::DoReadSIO(MemoryAccessSize size, u32 offset) { return g_sio.ReadRegister(offset); }

void Bus::DoWriteSIO(MemoryAccessSize size, u32 offset, u32 value) { g_sio.WriteRegister(offset, value); }

u32 Bus::DoReadCDROM(MemoryAccessSize size, u32 offset)
{
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      const u32 b0 = ZeroExtend32(g_cdrom.ReadRegister(offset));
      const u32 b1 = ZeroExtend32(g_cdrom.ReadRegister(offset + 1u));
      const u32 b2 = ZeroExtend32(g_cdrom.ReadRegister(offset + 2u));
      const u32 b3 = ZeroExtend32(g_cdrom.ReadRegister(offset + 3u));
      return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    case MemoryAccessSize::HalfWord:
    {
      const u32 lsb = ZeroExtend32(g_cdrom.ReadRegister(offset));
      const u32 msb = ZeroExtend32(g_cdrom.ReadRegister(offset + 1u));
      return lsb | (msb << 8);
    }

    case MemoryAccessSize::Byte:
    default:
      return ZeroExtend32(g_cdrom.ReadRegister(offset));
  }
}

void Bus::DoWriteCDROM(MemoryAccessSize size, u32 offset, u32 value)
{
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      g_cdrom.WriteRegister(offset, Truncate8(value & 0xFFu));
      g_cdrom.WriteRegister(offset + 1u, Truncate8((value >> 8) & 0xFFu));
      g_cdrom.WriteRegister(offset + 2u, Truncate8((value >> 16) & 0xFFu));
      g_cdrom.WriteRegister(offset + 3u, Truncate8((value >> 24) & 0xFFu));
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      g_cdrom.WriteRegister(offset, Truncate8(value & 0xFFu));
      g_cdrom.WriteRegister(offset + 1u, Truncate8((value >> 8) & 0xFFu));
    }
    break;

    case MemoryAccessSize::Byte:
    default:
      return g_cdrom.WriteRegister(offset, Truncate8(value));
  }
}

u32 Bus::DoReadGPU(MemoryAccessSize size, u32 offset)
{
  u32 value = g_gpu->ReadRegister(offset);
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteGPU(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  g_gpu->WriteRegister(offset, value);
}

u32 Bus::DoReadMDEC(MemoryAccessSize size, u32 offset)
{
  u32 value = g_mdec.ReadRegister(offset);
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteMDEC(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  g_mdec.WriteRegister(offset, value);
}

u32 Bus::DoReadInterruptController(MemoryAccessSize size, u32 offset)
{
  u32 value = g_interrupt_controller.ReadRegister(offset);
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteInterruptController(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  g_interrupt_controller.WriteRegister(offset, value);
}

u32 Bus::DoReadTimers(MemoryAccessSize size, u32 offset)
{
  u32 value = g_timers.ReadRegister(offset);
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteTimers(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  g_timers.WriteRegister(offset, value);
}

u32 Bus::DoReadSPU(MemoryAccessSize size, u32 offset)
{
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      // 32-bit reads are read as two 16-bit accesses.
      const u16 lsb = g_spu.ReadRegister(offset);
      const u16 msb = g_spu.ReadRegister(offset + 2);
      return ZeroExtend32(lsb) | (ZeroExtend32(msb) << 16);
    }

    case MemoryAccessSize::HalfWord:
    {
      return ZeroExtend32(g_spu.ReadRegister(offset));
    }

    case MemoryAccessSize::Byte:
    default:
    {
      u16 value = g_spu.ReadRegister(FIXUP_HALFWORD_READ_OFFSET(offset));
      return FIXUP_HALFWORD_READ_VALUE(offset, value);
    }
  }
}

void Bus::DoWriteSPU(MemoryAccessSize size, u32 offset, u32 value)
{
  // 32-bit writes are written as two 16-bit writes.
  // TODO: Ignore if address is not aligned.
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      DebugAssert(Common::IsAlignedPow2(offset, 2));
      g_spu.WriteRegister(offset, Truncate16(value));
      g_spu.WriteRegister(offset + 2, Truncate16(value >> 16));
      return;
    }

    case MemoryAccessSize::HalfWord:
    {
      DebugAssert(Common::IsAlignedPow2(offset, 2));
      g_spu.WriteRegister(offset, Truncate16(value));
      return;
    }

    case MemoryAccessSize::Byte:
    {
      g_spu.WriteRegister(FIXUP_HALFWORD_READ_OFFSET(offset), Truncate16(FIXUP_HALFWORD_READ_VALUE(offset, value)));
      return;
    }
  }
}

u32 Bus::DoReadDMA(MemoryAccessSize size, u32 offset)
{
  return FIXUP_WORD_READ_VALUE(offset, g_dma.ReadRegister(FIXUP_WORD_READ_OFFSET(offset)));
}

void Bus::DoWriteDMA(MemoryAccessSize size, u32 offset, u32 value)
{
  switch (size)
  {
    case MemoryAccessSize::Byte:
    case MemoryAccessSize::HalfWord:
    {
      // zero extend length register
      if ((offset & u32(0xF0)) < 7 && (offset & u32(0x0F)) == 0x4)
        value = ZeroExtend32(value);
      else
        FixupUnalignedWordAccessW32(offset, value);
    }

    default:
      break;
  }

  g_dma.WriteRegister(offset, value);
}

namespace CPU {

// defined in cpu_core.cpp
void RaiseException(Exception excode);
void RaiseException(Exception excode, u32 EPC, bool BD, bool BT, u8 CE);

static void WriteCacheControl(u32 value)
{
  Log_WarningPrintf("Cache control <- 0x%08X", value);
  g_state.cache_control = value;
}

template<MemoryAccessType type, MemoryAccessSize size>
static void DoScratchpadAccess(PhysicalMemoryAddress address, u32& value)
{
  const PhysicalMemoryAddress cache_offset = address & DCACHE_OFFSET_MASK;
  if constexpr (size == MemoryAccessSize::Byte)
  {
    if constexpr (type == MemoryAccessType::Read)
      value = ZeroExtend32(g_state.dcache[cache_offset]);
    else
      g_state.dcache[cache_offset] = Truncate8(value);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      u16 temp;
      std::memcpy(&temp, &g_state.dcache[cache_offset], sizeof(temp));
      value = ZeroExtend32(temp);
    }
    else
    {
      u16 temp = Truncate16(value);
      std::memcpy(&g_state.dcache[cache_offset], &temp, sizeof(temp));
    }
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if constexpr (type == MemoryAccessType::Read)
      std::memcpy(&value, &g_state.dcache[cache_offset], sizeof(value));
    else
      std::memcpy(&g_state.dcache[cache_offset], &value, sizeof(value));
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
static TickCount DoMemoryAccess(VirtualMemoryAddress address, u32& value)
{
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (g_state.cop0_regs.sr.Isc)
          return 0;
      }

      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(phys_addr, value);
        return 0;
      }

      return g_bus->DispatchAccess<type, size>(phys_addr, value);
    }

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    {
      // Above 512mb raises an exception.
      return -1;
    }

    case 0x04: // KSEG0 - physical memory cached
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (g_state.cop0_regs.sr.Isc)
          return 0;
      }

      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(phys_addr, value);
        return 0;
      }

      return g_bus->DispatchAccess<type, size>(phys_addr, value);
    }
    break;

    case 0x05: // KSEG1 - physical memory uncached
    {
      const PhysicalMemoryAddress phys_addr = address & UINT32_C(0x1FFFFFFF);
      return g_bus->DispatchAccess<type, size>(phys_addr, value);
    }
    break;

    case 0x06: // KSEG2
    case 0x07: // KSEG2
    {
      if (address == 0xFFFE0130)
      {
        if constexpr (type == MemoryAccessType::Read)
          value = g_state.cache_control;
        else
          WriteCacheControl(value);

        return 0;
      }
      else
      {
        return -1;
      }
    }

    default:
      UnreachableCode();
      return false;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
static bool DoAlignmentCheck(VirtualMemoryAddress address)
{
  if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if (Common::IsAlignedPow2(address, 2))
      return true;
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if (Common::IsAlignedPow2(address, 4))
      return true;
  }
  else
  {
    return true;
  }

  g_state.cop0_regs.BadVaddr = address;
  RaiseException(type == MemoryAccessType::Read ? Exception::AdEL : Exception::AdES);
  return false;
}

bool FetchInstruction()
{
  DebugAssert(Common::IsAlignedPow2(g_state.regs.npc, 4));
  if (DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(g_state.regs.npc, g_state.next_instruction.bits) <
      0)
  {
    // Bus errors don't set BadVaddr.
    RaiseException(Exception::IBE, g_state.regs.npc, false, false, 0);
    return false;
  }

  g_state.regs.pc = g_state.regs.npc;
  g_state.regs.npc += sizeof(g_state.next_instruction.bits);
  return true;
}

bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp);
  *value = Truncate8(temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  g_state.pending_ticks += cycles;
  return true;
}

bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp);
  *value = Truncate16(temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  g_state.pending_ticks += cycles;
  return true;
}

bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(addr))
    return false;

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  g_state.pending_ticks += cycles;
  return true;
}

bool WriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = ZeroExtend32(value);
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(addr))
    return false;

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp);
  *value = Truncate8(temp);
  return (cycles >= 0);
}

bool SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp);
  *value = Truncate16(temp);
  return (cycles >= 0);
}

bool SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  return DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value) >= 0;
}

bool SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, temp) >= 0;
}

bool SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, temp) >= 0;
}

bool SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value) >= 0;
}

namespace Recompiler::Thunks {

u64 ReadMemoryByte(u32 pc, u32 address)
{
  g_state.current_instruction_pc = pc;

  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  g_state.pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u64 ReadMemoryHalfWord(u32 pc, u32 address)
{
  g_state.current_instruction_pc = pc;

  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address))
    return UINT64_C(0xFFFFFFFFFFFFFFFF);

  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  g_state.pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u64 ReadMemoryWord(u32 pc, u32 address)
{
  g_state.current_instruction_pc = pc;

  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(address))
    return UINT64_C(0xFFFFFFFFFFFFFFFF);

  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return UINT64_C(0xFFFFFFFFFFFFFFFF);
  }

  g_state.pending_ticks += cycles;
  return ZeroExtend64(temp);
}

bool WriteMemoryByte(u32 pc, u32 address, u8 value)
{
  g_state.current_instruction_pc = pc;

  u32 temp = ZeroExtend32(value);
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(address, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool WriteMemoryHalfWord(u32 pc, u32 address, u16 value)
{
  g_state.current_instruction_pc = pc;

  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(address))
    return false;

  u32 temp = ZeroExtend32(value);
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(address, temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool WriteMemoryWord(u32 pc, u32 address, u32 value)
{
  g_state.current_instruction_pc = pc;

  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(address))
    return false;

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

} // namespace Recompiler::Thunks

} // namespace CPU