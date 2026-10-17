#include "application/boot.h"

#include "domain/ctr/backwards_lzss.h"
#include "domain/ctr/exefs.h"
#include "domain/ctr/ncch.h"
#include "domain/ctr/ncsd.h"
#include "domain/ctr/romfs.h"
#include "domain/kernel/executable_loader.h"
#include "domain/services/apt.h"
#include "domain/services/configuration.h"
#include "domain/services/digital_signal_processor.h"
#include "domain/services/file_system.h"
#include "domain/services/friends.h"
#include "domain/services/graphics.h"
#include "domain/services/human_interface.h"
#include "domain/services/network_daemons.h"
#include "domain/services/power_manager.h"
#include "domain/services/service_manager.h"
#include "domain/services/spot_pass.h"

#include <string>

namespace hexlith::application {
namespace {

namespace ctr = domain::ctr;
namespace kernel = domain::kernel;
namespace services = domain::services;

kernel::KernelConfig ConfigFor(const ctr::ExHeader& header) {
    kernel::KernelConfig config;
    config.linear_heap_base = kernel::layout::LinearHeapBaseFor(header.kernel.kernel_version);
    config.handle_capacity = header.kernel.handle_table_size;
    config.ideal_processor = header.ideal_processor;
    return config;
}

std::vector<std::string> AllowedServices(const ctr::ExHeader& header) {
    std::vector<std::string> names = header.services;
    names.insert(names.end(), header.extended_services.begin(), header.extended_services.end());
    return names;
}

// The parts of the executable partition the process is made from.
struct Executable {
    ctr::ExHeader header;
    std::vector<std::uint8_t> code;
    core::ByteSpan romfs;
};

core::Result<std::vector<std::uint8_t>>
ReadCode(core::ByteSpan partition, const ctr::NcchHeader& ncch, const ctr::ExHeader& header) {
    const core::Result<core::ByteSpan> region = ctr::RegionContents(partition, ncch.exefs, "ExeFS");
    if (!region) {
        return region.Failure();
    }
    const core::Result<ctr::ExeFs> exefs = ctr::ParseExeFs(region.Value());
    if (!exefs) {
        return exefs.Failure();
    }
    for (const ctr::ExeFsFile& file : exefs.Value().files) {
        if (file.name != ctr::exefs::CodeFileName) {
            continue;
        }
        const core::Result<core::ByteSpan> contents = ctr::ExeFsFileContents(region.Value(), file);
        if (!contents) {
            return contents.Failure();
        }
        if (!header.compress_exefs_code) {
            return std::vector<std::uint8_t>(contents.Value().begin(), contents.Value().end());
        }
        return ctr::DecompressBackwardsLzss(contents.Value());
    }
    return core::Error{"the ExeFS holds no .code"};
}

// Level 3 of the RomFS, which is what fs:USER serves as the title's RomFS.
core::Result<core::ByteSpan> ReadRomFs(core::ByteSpan partition, const ctr::NcchHeader& ncch) {
    const core::Result<core::ByteSpan> region = ctr::RegionContents(partition, ncch.romfs, "RomFS");
    if (!region) {
        return region.Failure();
    }
    const core::Result<ctr::RomFs> romfs = ctr::ParseRomFs(region.Value());
    if (!romfs) {
        return romfs.Failure();
    }
    return region.Value().subspan(romfs.Value().level3_offset);
}

core::Result<Executable> ReadExecutable(core::ByteSpan image, std::size_t partition_index) {
    const core::Result<ctr::NcsdHeader> ncsd = ctr::ParseNcsdHeader(image);
    if (!ncsd) {
        return ncsd.Failure().WithContext("the cartridge image");
    }
    const core::Result<core::ByteSpan> partition =
        ctr::PartitionContents(image, ncsd.Value(), partition_index);
    if (!partition) {
        return partition.Failure();
    }
    const core::Result<ctr::NcchHeader> ncch = ctr::ParseNcchHeader(partition.Value());
    if (!ncch) {
        return ncch.Failure();
    }
    const core::Result<core::ByteSpan> exheader_region =
        ctr::RegionContents(partition.Value(), ncch.Value().exheader, "extended header");
    if (!exheader_region) {
        return exheader_region.Failure();
    }
    core::Result<ctr::ExHeader> header = ctr::ParseExHeader(exheader_region.Value());
    if (!header) {
        return header.Failure();
    }

    core::Result<std::vector<std::uint8_t>> code =
        ReadCode(partition.Value(), ncch.Value(), header.Value());
    if (!code) {
        return code.Failure().WithContext("the .code section");
    }
    const core::Result<core::ByteSpan> romfs = ReadRomFs(partition.Value(), ncch.Value());
    if (!romfs) {
        return romfs.Failure();
    }
    return Executable{.header = std::move(header).Value(),
                      .code = std::move(code).Value(),
                      .romfs = romfs.Value()};
}

}  // namespace

BootedTitle::BootedTitle(domain::ctr::ExHeader header, core::ByteSpan romfs)
    : header_(std::move(header)),
      romfs_(romfs),
      kernel_(ConfigFor(header_)),
      registry_(AllowedServices(header_)),
      save_(std::make_shared<services::SaveData>()),
      extra_(std::make_shared<services::ExtraData>()),
      loader_(std::make_shared<services::RelocatableObjects>(kernel_)) {}

core::Result<std::unique_ptr<BootedTitle>> BootedTitle::FromCartridge(core::ByteSpan image,
                                                                      std::size_t partition) {
    core::Result<Executable> executable = ReadExecutable(image, partition);
    if (!executable) {
        return executable.Failure();
    }
    // The constructor is private, so make_unique cannot reach it.
    std::unique_ptr<BootedTitle> title{
        new BootedTitle(std::move(executable.Value().header), executable.Value().romfs)};
    const core::Status started = title->Start(executable.Value().code);
    if (!started) {
        return started.Failure();
    }
    return title;
}

core::Status BootedTitle::Start(core::ByteSpan code) {
    const core::Result<kernel::MainThreadStart> start =
        kernel::LoadExecutable(kernel_.Space(), header_, code, kernel_.Pages());
    if (!start) {
        return start.Failure();
    }
    kernel_.Start(start.Value());
    kernel_.MapDeviceMemory(header_.kernel.mapped_ranges);

    registry_.Provide(std::make_shared<services::AppletManager>(kernel_));
    registry_.Provide(std::make_shared<services::NetworkDaemonManager>());
    registry_.Provide(std::make_shared<services::FileSystem>(romfs_, save_, extra_));
    registry_.Provide(std::make_shared<services::Configuration>());
    registry_.Provide(std::make_shared<services::HumanInterface>(kernel_));
    graphics_ = std::make_shared<services::Graphics>(kernel_);
    registry_.Provide(graphics_);
    registry_.Provide(std::make_shared<services::DigitalSignalProcessor>(kernel_));
    registry_.Provide(std::make_shared<services::Friends>());
    registry_.Provide(std::make_shared<services::SpotPass>());
    registry_.Provide(std::make_shared<services::PowerManager>());
    registry_.Provide(loader_);
    kernel_.RegisterPort("srv:", std::make_shared<services::ServiceManager>(registry_));
    return {};
}

MemoryComparison CompareMemory(const domain::memory::ProcessMemory& memory,
                               std::uint32_t address,
                               core::ByteSpan expected) {
    MemoryComparison comparison;
    std::vector<std::uint8_t> actual(expected.size());
    comparison.readable = memory.Copy(address, actual).Ok();
    if (!comparison.readable) {
        return comparison;
    }
    for (std::size_t offset = 0; offset < expected.size(); ++offset) {
        if (actual.at(offset) != expected[offset]) {
            ++comparison.differing;
            if (!comparison.first_difference.has_value()) {
                comparison.first_difference = offset;
            }
        }
    }
    return comparison;
}

}  // namespace hexlith::application
