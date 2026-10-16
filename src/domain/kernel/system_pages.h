#ifndef HEXLITH_DOMAIN_KERNEL_SYSTEM_PAGES_H
#define HEXLITH_DOMAIN_KERNEL_SYSTEM_PAGES_H

#include "domain/memory/process_memory.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hexlith::domain::kernel {

// The sizes of the three FCRAM regions in one of the Old 3DS memory modes.
// Source: memory_region_sizes in Azahar, src/core/hle/kernel/memory.cpp, and
// 3dbrew, Memory layout.
struct MemoryRegionSizes {
    std::uint32_t application = 0;
    std::uint32_t system = 0;
    std::uint32_t base = 0;
};

// Mode 0, which the extended header's flag0 asks for in every retail title,
// the reference one included: 64 MiB for the application.
constexpr std::uint8_t ProductionMemoryMode = 0;
constexpr MemoryRegionSizes ProductionMemoryRegions{
    .application = 0x04000000, .system = 0x02C00000, .base = 0x01400000};

// The two read-only pages the kernel maps into every process: the
// configuration memory, which describes the kernel and the memory layout, and
// the shared page, which carries the time, the 3D slider and similar live
// state. 3dbrew, Configuration Memory, has both layouts.
//
// The values are the ones Azahar puts there, from firmware 11.17.0-50E
// (src/core/hle/kernel/config_mem.cpp and shared_page.cpp), so that the game
// reads what it read under Azahar. The time fields of the shared page are left
// at zero here and belong to the clock.
struct SystemPages {
    std::shared_ptr<memory::Backing> configuration;
    std::shared_ptr<memory::Backing> shared;
};

namespace config_memory {

constexpr std::size_t KernelVersionMinor = 0x02;
constexpr std::size_t KernelVersionMajor = 0x03;
constexpr std::size_t HomeMenuTitleId = 0x08;
constexpr std::size_t SystemCoreVersion = 0x10;
constexpr std::size_t UnitInfo = 0x14;
constexpr std::size_t PreviousFirm = 0x16;
constexpr std::size_t SdkVersion = 0x18;
constexpr std::size_t ApplicationMemoryType = 0x30;
constexpr std::size_t ApplicationMemoryAllocated = 0x40;
constexpr std::size_t SystemMemoryAllocated = 0x44;
constexpr std::size_t BaseMemoryAllocated = 0x48;
constexpr std::size_t FirmVersionMinor = 0x62;
constexpr std::size_t FirmVersionMajor = 0x63;
constexpr std::size_t FirmSystemCoreVersion = 0x64;
constexpr std::size_t FirmSdkVersion = 0x68;

}  // namespace config_memory

namespace shared_page {

constexpr std::size_t RunningHardware = 0x04;
constexpr std::size_t UnknownFlag = 0x86;

}  // namespace shared_page

[[nodiscard]] SystemPages MakeSystemPages(std::uint8_t memory_mode, const MemoryRegionSizes& sizes);

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_SYSTEM_PAGES_H
