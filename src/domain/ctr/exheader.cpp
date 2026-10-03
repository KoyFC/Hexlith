#include "domain/ctr/exheader.h"

#include <format>

namespace hexlith::domain::ctr {
namespace {

core::Result<CodeSetInfo>
ReadCodeSet(core::ByteSpan region, std::size_t offset, std::string_view what) {
    CodeSetInfo segment;
    segment.address = core::ReadU32At(region, offset + exheader::CodeSetAddressOffset).ValueOr(0);
    segment.page_count =
        core::ReadU32At(region, offset + exheader::CodeSetPageCountOffset).ValueOr(0);
    segment.size = core::ReadU32At(region, offset + exheader::CodeSetSizeOffset).ValueOr(0);

    // A segment whose contents do not fit in the pages reserved for it is
    // not a layout the loader could have used, so the field was misread or
    // the header is not one.
    if (segment.size > segment.PhysicalRegionSize()) {
        return core::Error{
            std::format("the {} segment declares {:#x} bytes in {} page(s), which hold {:#x}",
                        what,
                        segment.size,
                        segment.page_count,
                        segment.PhysicalRegionSize())};
    }

    return segment;
}

// The lists in the extended header are fixed-width arrays with their unused
// entries zeroed, so a zero is the end of the list rather than a member of
// it.
std::vector<std::uint64_t> ReadDependencies(core::ByteSpan region) {
    std::vector<std::uint64_t> dependencies;
    for (std::size_t index = 0; index < exheader::DependencyCount; ++index) {
        const std::uint64_t title_id =
            core::ReadU64At(region, exheader::DependencyListOffset + (index * 8)).ValueOr(0);
        if (title_id == 0) {
            break;
        }
        dependencies.push_back(title_id);
    }
    return dependencies;
}

core::Result<std::vector<std::string>>
ReadServices(core::ByteSpan region, std::size_t offset, std::size_t count, std::string_view what) {
    std::vector<std::string> services;
    for (std::size_t index = 0; index < count; ++index) {
        const core::Result<std::string> name = core::ReadFixedAsciiAt(
            region, offset + (index * exheader::ServiceNameSize), exheader::ServiceNameSize);
        if (!name) {
            return name.Failure().WithContext(std::format("{} entry {}", what, index));
        }
        if (name.Value().empty()) {
            break;
        }
        services.push_back(name.Value());
    }
    return services;
}

}  // namespace

core::Result<ExHeader> ParseExHeader(core::ByteSpan region) {
    if (region.size() < exheader::SizeOnDisk) {
        return core::Error{
            std::format("an extended header occupies {:#x} bytes on disk and the region is {:#x}",
                        exheader::SizeOnDisk,
                        region.size())};
    }

    ExHeader header;

    const core::Result<std::string> title = core::ReadFixedAsciiAt(
        region, exheader::ApplicationTitleOffset, exheader::ApplicationTitleSize);
    if (!title) {
        return title.Failure().WithContext("the application title");
    }
    header.application_title = title.Value();

    const std::uint8_t flags = core::ReadU8At(region, exheader::FlagsOffset).Value();
    header.compress_exefs_code = (flags & exheader::CompressExefsCodeFlag) != 0;
    header.sd_application = (flags & exheader::SdApplicationFlag) != 0;
    header.remaster_version = core::ReadU16At(region, exheader::RemasterVersionOffset).Value();

    const core::Result<CodeSetInfo> text = ReadCodeSet(region, exheader::TextCodeSetOffset, "text");
    if (!text) {
        return text.Failure();
    }
    header.text = text.Value();

    const core::Result<CodeSetInfo> read_only =
        ReadCodeSet(region, exheader::ReadOnlyCodeSetOffset, "read-only");
    if (!read_only) {
        return read_only.Failure();
    }
    header.read_only = read_only.Value();

    const core::Result<CodeSetInfo> data = ReadCodeSet(region, exheader::DataCodeSetOffset, "data");
    if (!data) {
        return data.Failure();
    }
    header.data = data.Value();

    // An executable has code. A header reporting none was either read at the
    // wrong offset or does not belong to an executable partition, and both
    // are worth saying out loud before anything tries to lift it.
    if (!header.text.Present()) {
        return core::Error{"the extended header declares no text segment"};
    }

    header.stack_size = core::ReadU32At(region, exheader::StackSizeOffset).Value();
    header.bss_size = core::ReadU32At(region, exheader::BssSizeOffset).Value();
    header.dependencies = ReadDependencies(region);
    header.save_data_size = core::ReadU64At(region, exheader::SaveDataSizeOffset).Value();
    header.jump_id = core::ReadU64At(region, exheader::JumpIdOffset).Value();

    header.program_id = core::ReadU64At(region, exheader::AciProgramIdOffset).Value();
    header.core_version = core::ReadU32At(region, exheader::AciCoreVersionOffset).Value();
    header.main_thread_priority = core::ReadU8At(region, exheader::AciPriorityOffset).Value();

    const core::Result<std::vector<std::string>> services =
        ReadServices(region,
                     exheader::ServiceAccessControlOffset,
                     exheader::ServiceAccessControlCount,
                     "the service access control list");
    if (!services) {
        return services.Failure();
    }
    header.services = services.Value();

    const std::size_t extended_count =
        (exheader::ResourceLimitCategoryOffset - exheader::ExtendedServiceAccessControlOffset) /
        exheader::ServiceNameSize;
    const core::Result<std::vector<std::string>> extended =
        ReadServices(region,
                     exheader::ExtendedServiceAccessControlOffset,
                     extended_count,
                     "the extended service access control list");
    if (!extended) {
        return extended.Failure();
    }
    header.extended_services = extended.Value();

    header.limitation_program_id = core::ReadU64At(region, exheader::LimitationAciOffset).Value();

    return header;
}

}  // namespace hexlith::domain::ctr
