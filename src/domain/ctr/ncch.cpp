#include "domain/ctr/ncch.h"

#include <array>
#include <format>

namespace hexlith::domain::ctr {
namespace {

// A region whose offset and size fields are a pair of consecutive words, both
// counts of media units.
core::Result<NcchRegion> ReadRegion(core::ByteSpan partition,
                                    std::size_t offset_field,
                                    std::size_t size_field,
                                    std::string_view what) {
    const core::Result<std::uint32_t> offset_units = core::ReadU32At(partition, offset_field);
    if (!offset_units) {
        return offset_units.Failure().WithContext(std::format("the {} offset", what));
    }
    const core::Result<std::uint32_t> size_units = core::ReadU32At(partition, size_field);
    if (!size_units) {
        return size_units.Failure().WithContext(std::format("the {} size", what));
    }

    NcchRegion region;
    region.offset = BytesFromMediaUnits(offset_units.Value());
    region.size = BytesFromMediaUnits(size_units.Value());

    if (region.size > 0 && region.offset == 0) {
        return core::Error{
            std::format("the {} declares {:#x} bytes at offset zero, which is where the NCCH "
                        "header is",
                        what,
                        region.size)};
    }
    if (region.size > 0 && region.offset + region.size > partition.size()) {
        return core::Error{
            std::format("the {} runs from {:#x} for {:#x} bytes, past the end of a partition of "
                        "{:#x} bytes",
                        what,
                        region.offset,
                        region.size,
                        partition.size())};
    }

    return region;
}

core::Status ReadDigestInto(core::ByteSpan partition,
                            std::size_t offset,
                            core::Digest& digest,
                            std::string_view what) {
    const core::Result<core::Digest> stored = core::ReadDigestAt(partition, offset);
    if (!stored) {
        return stored.Failure().WithContext(std::format("the {} hash", what));
    }
    digest = stored.Value();
    return {};
}

}  // namespace

core::Result<NcchHeader> ParseNcchHeader(core::ByteSpan partition) {
    if (partition.size() < ncch::HeaderSize) {
        return core::Error{std::format("an NCCH header is {:#x} bytes and the partition is {:#x}",
                                       ncch::HeaderSize,
                                       partition.size())};
    }

    if (!core::MatchesAt(partition, ncch::MagicOffset, ncch::Magic)) {
        return core::Error{std::format(
            "there is no NCCH magic at offset {:#x}, so this is not a content partition",
            ncch::MagicOffset)};
    }

    NcchHeader header;
    header.content_size =
        BytesFromMediaUnits(core::ReadU32At(partition, ncch::ContentSizeOffset).Value());
    header.partition_id = core::ReadU64At(partition, ncch::PartitionIdOffset).Value();
    header.maker_code = core::ReadU16At(partition, ncch::MakerCodeOffset).Value();
    header.format_version = core::ReadU16At(partition, ncch::VersionOffset).Value();
    header.program_id = core::ReadU64At(partition, ncch::ProgramIdOffset).Value();

    const core::Result<std::string> product_code =
        core::ReadFixedAsciiAt(partition, ncch::ProductCodeOffset, ncch::ProductCodeSize);
    if (!product_code) {
        return product_code.Failure().WithContext("the product code");
    }
    header.product_code = product_code.Value();

    const core::Result<core::ByteSpan> flags =
        core::ReadBytesAt(partition, ncch::FlagsOffset, ncch::FlagsSize);
    if (!flags) {
        return flags.Failure().WithContext("the flags");
    }
    header.flags.crypto_method = flags.Value()[ncch::CryptoMethodByte];
    header.flags.content_platform = flags.Value()[ncch::ContentPlatformByte];
    header.flags.content_type = flags.Value()[ncch::ContentTypeByte];
    header.flags.content_unit_size = flags.Value()[ncch::ContentUnitSizeByte];
    header.flags.bitmask = flags.Value()[ncch::BitmaskByte];

    if (!header.flags.NoCrypto()) {
        return core::Error{std::format(
            "the partition does not have the NoCrypto flag set, so it is encrypted with crypto "
            "method {:#04x}. Hexlith handles no keys and implements no decryption, so this is a "
            "limit of the project and not a missing feature",
            header.flags.crypto_method)};
    }

    if (header.content_size > partition.size()) {
        return core::Error{
            std::format("the NCCH header declares {:#x} bytes of content and the partition holds "
                        "{:#x}, so it is truncated",
                        header.content_size,
                        partition.size())};
    }

    header.declared_exheader_size = core::ReadU32At(partition, ncch::ExHeaderSizeOffset).Value();
    if (header.declared_exheader_size != 0 &&
        header.declared_exheader_size != exheader::HashedSize) {
        return core::Error{std::format(
            "the extended header size field reads {:#x}. A title with an extended header reports "
            "{:#x}, which is the half its hash covers, and one without reports zero",
            header.declared_exheader_size,
            exheader::HashedSize)};
    }
    if (header.declared_exheader_size != 0) {
        // The region on disk is twice the declared size. Reporting the region
        // rather than the field is what keeps the discrepancy from being
        // rediscovered as a bug by whoever reads the extended header next.
        header.exheader.offset = ncch::HeaderSize;
        header.exheader.size = exheader::SizeOnDisk;
        if (header.exheader.offset + header.exheader.size > partition.size()) {
            return core::Error{std::format(
                "the extended header needs {:#x} bytes after the header and the partition holds "
                "{:#x}",
                exheader::SizeOnDisk,
                partition.size())};
        }
    }

    struct RegionField {
        NcchRegion* region;
        std::size_t offset_field;
        std::size_t size_field;
        std::string_view name;
    };

    const std::array<RegionField, 4> regions = {{
        {.region = &header.plain,
         .offset_field = ncch::PlainRegionOffsetOffset,
         .size_field = ncch::PlainRegionSizeOffset,
         .name = "plain region"},
        {.region = &header.logo,
         .offset_field = ncch::LogoRegionOffsetOffset,
         .size_field = ncch::LogoRegionSizeOffset,
         .name = "logo region"},
        {.region = &header.exefs,
         .offset_field = ncch::ExeFsOffsetOffset,
         .size_field = ncch::ExeFsSizeOffset,
         .name = "ExeFS"},
        {.region = &header.romfs,
         .offset_field = ncch::RomFsOffsetOffset,
         .size_field = ncch::RomFsSizeOffset,
         .name = "RomFS"},
    }};

    for (const RegionField& field : regions) {
        const core::Result<NcchRegion> region =
            ReadRegion(partition, field.offset_field, field.size_field, field.name);
        if (!region) {
            return region.Failure();
        }
        *field.region = region.Value();
    }

    header.exefs_hash_region_size =
        BytesFromMediaUnits(core::ReadU32At(partition, ncch::ExeFsHashRegionSizeOffset).Value());
    header.romfs_hash_region_size =
        BytesFromMediaUnits(core::ReadU32At(partition, ncch::RomFsHashRegionSizeOffset).Value());

    struct HashField {
        std::size_t offset;
        core::Digest* digest;
        std::string_view name;
    };

    const std::array<HashField, 4> hashes = {{
        {.offset = ncch::ExHeaderHashOffset,
         .digest = &header.exheader_hash,
         .name = "extended header"},
        {.offset = ncch::LogoHashOffset, .digest = &header.logo_hash, .name = "logo region"},
        {.offset = ncch::ExeFsSuperblockHashOffset,
         .digest = &header.exefs_superblock_hash,
         .name = "ExeFS superblock"},
        {.offset = ncch::RomFsSuperblockHashOffset,
         .digest = &header.romfs_superblock_hash,
         .name = "RomFS superblock"},
    }};

    for (const HashField& field : hashes) {
        const core::Status stored =
            ReadDigestInto(partition, field.offset, *field.digest, field.name);
        if (!stored) {
            return stored.Failure();
        }
    }

    return header;
}

core::Result<core::ByteSpan>
RegionContents(core::ByteSpan partition, const NcchRegion& region, std::string_view what) {
    if (!region.Present()) {
        return core::Error{std::format("the {} is absent from this partition", what)};
    }

    const core::Result<core::ByteSpan> contents =
        core::ReadBytesAt(partition, region.offset, region.size);
    if (!contents) {
        return contents.Failure().WithContext(std::string{what});
    }
    return contents.Value();
}

}  // namespace hexlith::domain::ctr
