#ifndef HEXLITH_APPLICATION_BOOT_H
#define HEXLITH_APPLICATION_BOOT_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/exheader.h"
#include "domain/kernel/kernel.h"
#include "domain/services/registry.h"
#include "domain/services/relocatable_objects.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hexlith::application {

// The title's process as the console starts it: its executable loaded from the
// cartridge into a fresh address space, its main thread at the entry point, the
// device memory its extended header grants mapped, and srv: answering for
// every service the boot reaches. Nothing has run yet.
//
// The cartridge image outlives the process, which serves the RomFS straight
// out of it.
class BootedTitle {
public:
    // From the executable partition of a cartridge image: its extended header,
    // its .code, decompressed when the header says so, and level 3 of its
    // RomFS.
    static core::Result<std::unique_ptr<BootedTitle>> FromCartridge(core::ByteSpan image,
                                                                    std::size_t partition);

    BootedTitle(const BootedTitle&) = delete;
    BootedTitle& operator=(const BootedTitle&) = delete;
    BootedTitle(BootedTitle&&) = delete;
    BootedTitle& operator=(BootedTitle&&) = delete;
    ~BootedTitle() = default;

    domain::kernel::Kernel& Kernel() {
        return kernel_;
    }

    [[nodiscard]] const domain::ctr::ExHeader& Header() const {
        return header_;
    }

    // ldr:ro, for what it has loaded.
    [[nodiscard]] const domain::services::RelocatableObjects& Loader() const {
        return *loader_;
    }

private:
    BootedTitle(domain::ctr::ExHeader header, core::ByteSpan romfs);

    core::Status Start(core::ByteSpan code);

    domain::ctr::ExHeader header_;
    core::ByteSpan romfs_;
    domain::kernel::Kernel kernel_;
    domain::services::ServiceRegistry registry_;
    std::shared_ptr<domain::services::RelocatableObjects> loader_;
};

// How a range of the process's memory compares with bytes captured elsewhere,
// such as a dump of the same moment out of Azahar.
struct MemoryComparison {
    // Whether every byte of the range is mapped.
    bool readable = false;
    std::size_t differing = 0;
    std::optional<std::size_t> first_difference;
};

MemoryComparison CompareMemory(const domain::memory::ProcessMemory& memory,
                               std::uint32_t address,
                               core::ByteSpan expected);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_BOOT_H
