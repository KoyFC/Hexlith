#include "domain/kernel/system_pages.h"

#include <span>

namespace hexlith::domain::kernel {
namespace {

// Firmware 11.17.0-50E, the version Azahar describes itself as: kernel 2.58,
// system core version 2, SDK 0xF450, Home Menu as the NS title, a retail unit
// that booted NATIVE_FIRM before.
constexpr std::uint8_t KernelMinor = 0x3A;
constexpr std::uint8_t KernelMajor = 0x02;
constexpr std::uint64_t HomeMenuNsTitle = 0x0004013000008002;
constexpr std::uint32_t CoreVersion = 0x2;
constexpr std::uint8_t RetailUnit = 0x1;
constexpr std::uint8_t NativeFirm = 0x1;
constexpr std::uint32_t Sdk = 0x0000F450;

// The shared page says the console is a production unit, and Azahar sets one
// byte at 0x86 whose meaning nobody has documented.
constexpr std::uint8_t ProductHardware = 0x1;
constexpr std::uint8_t UnknownValue = 0x1;

template<typename Value>
void Put(std::span<std::uint8_t> page, std::size_t offset, Value value) {
    for (std::size_t byte = 0; byte < sizeof(Value); ++byte) {
        page[offset + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
    }
}

}  // namespace

SystemPages MakeSystemPages(std::uint8_t memory_mode, const MemoryRegionSizes& sizes) {
    SystemPages pages{.configuration = memory::Backing::Allocate(memory::ProcessMemory::PageSize),
                      .shared = memory::Backing::Allocate(memory::ProcessMemory::PageSize)};

    const std::span<std::uint8_t> config = pages.configuration->Bytes();
    Put(config, config_memory::KernelVersionMinor, KernelMinor);
    Put(config, config_memory::KernelVersionMajor, KernelMajor);
    Put(config, config_memory::HomeMenuTitleId, HomeMenuNsTitle);
    Put(config, config_memory::SystemCoreVersion, CoreVersion);
    Put(config, config_memory::UnitInfo, RetailUnit);
    Put(config, config_memory::PreviousFirm, NativeFirm);
    Put(config, config_memory::SdkVersion, Sdk);
    Put(config, config_memory::ApplicationMemoryType, static_cast<std::uint32_t>(memory_mode));
    Put(config, config_memory::ApplicationMemoryAllocated, sizes.application);
    Put(config, config_memory::SystemMemoryAllocated, sizes.system);
    Put(config, config_memory::BaseMemoryAllocated, sizes.base);
    Put(config, config_memory::FirmVersionMinor, KernelMinor);
    Put(config, config_memory::FirmVersionMajor, KernelMajor);
    Put(config, config_memory::FirmSystemCoreVersion, CoreVersion);
    Put(config, config_memory::FirmSdkVersion, Sdk);

    const std::span<std::uint8_t> shared = pages.shared->Bytes();
    Put(shared, shared_page::RunningHardware, ProductHardware);
    Put(shared, shared_page::UnknownFlag, UnknownValue);
    return pages;
}

}  // namespace hexlith::domain::kernel
