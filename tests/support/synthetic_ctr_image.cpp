#include "support/synthetic_ctr_image.h"

#include "core/sha256.h"
#include "domain/ctr/layout.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hexlith::test {
namespace {

namespace ctr = domain::ctr;

// Filler for the regions Hexlith never reads. A fixed pattern rather than
// anything random, because two runs of a test have to produce the same image.
constexpr std::uint8_t SignatureFiller = 0x5A;
constexpr std::uint8_t PaddingFiller = 0x00;

// Content for a partition the executable parser must not look inside, so
// that selecting the wrong one produces obvious nonsense.
constexpr std::uint8_t ExtraPartitionFiller = 0xCC;

constexpr std::uint64_t ByteMask = 0xFF;

// Plausible system module dependencies. Any two title IDs would do; these
// are shaped like the ones a retail title lists.
constexpr std::uint64_t FirstDependencyTitleId = 0x0004013000002F02;
constexpr std::uint64_t SecondDependencyTitleId = 0x0004013000001A02;

// One mebibyte, which is what the reference image declares.
constexpr std::uint64_t SaveDataSize = 0x100000;

constexpr std::uint8_t MainThreadPriority = 0x30;

// The limitation copy of the access control info carries a wildcard program
// ID, which is what identifies it as the access descriptor's copy rather
// than a duplicate of the real one. Observed on the reference image.
constexpr std::uint64_t WildcardProgramId = 0x00040000FFFFFF00;

// A CARD1 cartridge has no writable region, which it reports this way.
constexpr std::uint32_t Card1WritableAddress = 0xFFFFFFFF;

// Filesystem type 1 is what a decrypted retail partition reports.
constexpr std::uint8_t NormalFilesystemType = 1;

constexpr std::uint32_t AciCoreVersion = 2;

// One media unit of each filesystem is covered by its superblock hash, which
// is enough for a test and is what the field counts.
constexpr std::uint32_t HashedMediaUnits = 1;

// A prime, so the filler pattern never lines up with a power-of-two region
// boundary and a parser reading one region as another cannot produce bytes
// that look right.
constexpr std::size_t PatternPeriod = 251;

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

std::uint32_t InMediaUnits(std::size_t bytes) {
    return static_cast<std::uint32_t>(bytes / ctr::MediaUnit);
}

// Places little-endian fields at absolute offsets in a buffer that has
// already been sized. Writing by offset rather than sequentially is what
// lets the builder follow the documented layout tables line by line.
class Placer {
public:
    explicit Placer(std::vector<std::uint8_t>& bytes) : bytes_(&bytes) {}

    void U8(std::size_t offset, std::uint8_t value) {
        bytes_->at(offset) = value;
    }

    void U16(std::size_t offset, std::uint16_t value) {
        Integer(offset, value, 2);
    }

    void U32(std::size_t offset, std::uint32_t value) {
        Integer(offset, value, 4);
    }

    void U64(std::size_t offset, std::uint64_t value) {
        Integer(offset, value, 8);
    }

    void Bytes(std::size_t offset, core::ByteSpan data) {
        for (std::size_t index = 0; index < data.size(); ++index) {
            bytes_->at(offset + index) = data[index];
        }
    }

    void Text(std::size_t offset, std::string_view text, std::size_t field_size) {
        const std::size_t written = std::min(text.size(), field_size);
        for (std::size_t index = 0; index < written; ++index) {
            bytes_->at(offset + index) = static_cast<std::uint8_t>(text[index]);
        }
    }

    void Fill(std::size_t offset, std::size_t size, std::uint8_t value) {
        for (std::size_t index = 0; index < size; ++index) {
            bytes_->at(offset + index) = value;
        }
    }

    void Digest(std::size_t offset, core::ByteSpan over) {
        const core::Digest digest = core::Sha256::Of(over);
        Bytes(offset, core::ByteSpan{digest.bytes});
    }

private:
    void Integer(std::size_t offset, std::uint64_t value, std::size_t width) {
        for (std::size_t index = 0; index < width; ++index) {
            bytes_->at(offset + index) =
                static_cast<std::uint8_t>((value >> (8 * index)) & ByteMask);
        }
    }

    std::vector<std::uint8_t>* bytes_;
};

// The regions of the executable partition, in the order they are laid out.
struct PartitionLayout {
    std::size_t exheader = 0;
    std::size_t logo = 0;
    std::size_t logo_size = 0;
    std::size_t plain = 0;
    std::size_t plain_size = 0;
    std::size_t exefs = 0;
    std::size_t exefs_size = 0;
    std::size_t romfs = 0;
    std::size_t romfs_size = 0;
    std::size_t total = 0;
};

// The ExeFS data area, with each file aligned the way a real ExeFS aligns
// them. Offsets are relative to the end of the ExeFS header.
struct ExeFsLayout {
    std::vector<std::size_t> relative_offsets;
    std::size_t size = 0;
};

ExeFsLayout PlanExeFs(const std::vector<ExeFsFileSpec>& files) {
    ExeFsLayout layout;
    std::size_t next = 0;
    for (const ExeFsFileSpec& file : files) {
        layout.relative_offsets.push_back(next);
        next = AlignUp(next + file.data.size(), ctr::MediaUnit);
    }
    layout.size = ctr::exefs::HeaderSize + next;
    return layout;
}

PartitionLayout PlanPartition(const SyntheticImageSpec& spec, const ExeFsLayout& exefs) {
    PartitionLayout layout;
    layout.exheader = ctr::ncch::HeaderSize;

    std::size_t next = layout.exheader + ctr::exheader::SizeOnDisk;

    layout.logo_size = AlignUp(spec.logo.size(), ctr::MediaUnit);
    if (layout.logo_size > 0) {
        layout.logo = next;
        next += layout.logo_size;
    }

    layout.plain_size = AlignUp(spec.plain_region.size(), ctr::MediaUnit);
    if (layout.plain_size > 0) {
        layout.plain = next;
        next += layout.plain_size;
    }

    layout.exefs_size = exefs.relative_offsets.empty() ? 0 : AlignUp(exefs.size, ctr::MediaUnit);
    if (layout.exefs_size > 0) {
        layout.exefs = next;
        next += layout.exefs_size;
    }

    layout.romfs_size = AlignUp(spec.romfs.size(), ctr::MediaUnit);
    if (layout.romfs_size > 0) {
        layout.romfs = next;
        next += layout.romfs_size;
    }

    layout.total = next;
    return layout;
}

void WriteCodeSet(Placer& place, std::size_t offset, const CodeSetSpec& segment) {
    place.U32(offset + ctr::exheader::CodeSetAddressOffset, segment.address);
    place.U32(offset + ctr::exheader::CodeSetPageCountOffset, segment.pages);
    place.U32(offset + ctr::exheader::CodeSetSizeOffset, segment.size);
}

void WriteExHeader(Placer& place, std::size_t base, const SyntheticImageSpec& spec) {
    using namespace ctr::exheader;

    place.Text(base + ApplicationTitleOffset, spec.application_title, ApplicationTitleSize);

    std::uint8_t flags = 0;
    if (spec.compress_exefs_code) {
        flags |= CompressExefsCodeFlag;
    }
    place.U8(base + FlagsOffset, flags);
    place.U16(base + RemasterVersionOffset, spec.remaster_version);

    WriteCodeSet(place, base + TextCodeSetOffset, spec.text);
    WriteCodeSet(place, base + ReadOnlyCodeSetOffset, spec.read_only);
    WriteCodeSet(place, base + DataCodeSetOffset, spec.data);
    place.U32(base + StackSizeOffset, spec.stack_size);
    place.U32(base + BssSizeOffset, spec.bss_size);

    // Two dependencies, so that a parser reading the list finds a terminator
    // rather than running to the end of the field.
    place.U64(base + DependencyListOffset, FirstDependencyTitleId);
    place.U64(base + DependencyListOffset + 8, SecondDependencyTitleId);

    place.U64(base + SaveDataSizeOffset, SaveDataSize);
    place.U64(base + JumpIdOffset, spec.program_id);

    place.U64(base + AciProgramIdOffset, spec.program_id);
    place.U32(base + AciCoreVersionOffset, AciCoreVersion);
    place.U8(base + AciPriorityOffset, MainThreadPriority);

    for (std::size_t index = 0; index < std::min(spec.services.size(), ServiceAccessControlCount);
         ++index) {
        place.Text(base + ServiceAccessControlOffset + (index * ServiceNameSize),
                   spec.services[index],
                   ServiceNameSize);
    }

    // The access descriptor's two high-entropy blocks, and the limitation
    // copy of the access control info with the wildcard program ID that
    // identifies it as the descriptor's copy.
    place.Fill(base + AccessDescriptorOffset, AccessDescriptorSignatureSize, SignatureFiller);
    place.Fill(base + NcchPublicKeyOffset, NcchPublicKeySize, SignatureFiller);
    place.U64(base + LimitationAciOffset, WildcardProgramId);
    for (std::size_t index = 0; index < std::min(spec.services.size(), ServiceAccessControlCount);
         ++index) {
        place.Text(base + LimitationAciOffset +
                       (ServiceAccessControlOffset - AccessControlInfoOffset) +
                       (index * ServiceNameSize),
                   spec.services[index],
                   ServiceNameSize);
    }
}

void WriteExeFs(Placer& place,
                std::size_t base,
                const SyntheticImageSpec& spec,
                const ExeFsLayout& layout,
                SyntheticImage& image) {
    using namespace ctr::exefs;

    for (std::size_t index = 0; index < spec.exefs_files.size(); ++index) {
        const ExeFsFileSpec& file = spec.exefs_files[index];
        const std::size_t header = base + FileHeaderOffset + (index * FileHeaderSize);
        place.Text(header, file.name, FileNameSize);
        place.U32(header + FileOffsetOffset,
                  static_cast<std::uint32_t>(layout.relative_offsets[index]));
        place.U32(header + FileSizeOffset, static_cast<std::uint32_t>(file.data.size()));

        const std::size_t data = base + HeaderSize + layout.relative_offsets[index];
        place.Bytes(data, core::ByteSpan{file.data});
        image.exefs_file_offsets.push_back(data);

        // The hash table runs backwards against the file headers, so the
        // first file's hash is the last entry.
        const std::size_t hash =
            base + HashTableOffset + ((MaxFileCount - 1 - index) * ctr::Sha256Size);
        place.Digest(hash, core::ByteSpan{file.data});
    }
}

void WriteNcch(Placer& place,
               std::size_t base,
               const SyntheticImageSpec& spec,
               const PartitionLayout& layout) {
    using namespace ctr::ncch;

    place.Fill(base, ctr::SignatureSize, SignatureFiller);
    place.Text(base + MagicOffset, Magic, Magic.size());
    place.U32(base + ContentSizeOffset, InMediaUnits(layout.total));
    place.U64(base + PartitionIdOffset, spec.partition_id);
    place.U16(base + MakerCodeOffset, spec.maker_code);
    place.U16(base + VersionOffset, spec.ncch_version);
    place.U64(base + ProgramIdOffset, spec.program_id);
    place.Text(base + ProductCodeOffset, spec.product_code, ProductCodeSize);

    // The size field reports the hashed half, which is the discrepancy
    // docs/formats/exheader.md explains.
    place.U32(base + ExHeaderSizeOffset, static_cast<std::uint32_t>(ctr::exheader::HashedSize));

    place.U8(base + FlagsOffset + ContentPlatformByte, spec.content_platform);
    place.U8(base + FlagsOffset + ContentTypeByte, spec.content_type);
    place.U8(base + FlagsOffset + ContentUnitSizeByte, 0);
    place.U8(base + FlagsOffset + BitmaskByte, spec.no_crypto ? NoCryptoFlag : 0);

    place.U32(base + PlainRegionOffsetOffset, InMediaUnits(layout.plain));
    place.U32(base + PlainRegionSizeOffset, InMediaUnits(layout.plain_size));
    place.U32(base + LogoRegionOffsetOffset, InMediaUnits(layout.logo));
    place.U32(base + LogoRegionSizeOffset, InMediaUnits(layout.logo_size));
    place.U32(base + ExeFsOffsetOffset, InMediaUnits(layout.exefs));
    place.U32(base + ExeFsSizeOffset, InMediaUnits(layout.exefs_size));
    place.U32(base + RomFsOffsetOffset, InMediaUnits(layout.romfs));
    place.U32(base + RomFsSizeOffset, InMediaUnits(layout.romfs_size));

    // One media unit each, which is what the superblock hashes cover.
    place.U32(base + ExeFsHashRegionSizeOffset, layout.exefs_size > 0 ? HashedMediaUnits : 0);
    place.U32(base + RomFsHashRegionSizeOffset, layout.romfs_size > 0 ? HashedMediaUnits : 0);
}

void WriteNcsd(Placer& place,
               const SyntheticImageSpec& spec,
               std::size_t partition_offset,
               std::size_t partition_size,
               std::size_t image_size) {
    using namespace ctr::ncsd;

    place.Fill(0, ctr::SignatureSize, SignatureFiller);
    place.Text(MagicOffset, Magic, Magic.size());
    place.U32(ImageSizeOffset, InMediaUnits(image_size));
    place.U64(MediaIdOffset, spec.media_id);

    // Filesystem type 1 and crypt type 0 for the executable partition, which
    // is what a decrypted retail image reports.
    place.U8(PartitionFsTypesOffset + ExecutableContentPartition, NormalFilesystemType);

    place.U32(PartitionTableOffset + (ExecutableContentPartition * PartitionTableEntrySize),
              InMediaUnits(partition_offset));
    place.U32(PartitionTableOffset + (ExecutableContentPartition * PartitionTableEntrySize) + 4,
              InMediaUnits(partition_size));
    place.U64(PartitionIdTableOffset + (ExecutableContentPartition * 8), spec.partition_id);

    place.U32(AdditionalHeaderSizeOffset, 0);
    place.U32(SectorZeroOffsetOffset, 0);

    place.U32(card_info::WritableAddressOffset, Card1WritableAddress);
    place.U32(card_info::FilledSizeOffset, InMediaUnits(image_size));
    place.U16(card_info::TitleVersionOffset, spec.title_version);
    place.U16(card_info::CardRevisionOffset, 0);
    place.Fill(card_info::InitialDataOffset, card_info::InitialDataSize, SignatureFiller);
}

}  // namespace

std::vector<std::uint8_t> PatternBytes(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed + (index % PatternPeriod));
    }
    return bytes;
}

// Region sizes and filler seeds for the default image. Each region gets a
// different seed so that a parser reading one region where another belongs
// produces bytes that are obviously from the wrong place.
constexpr std::size_t DefaultLogoSize = 0x400;
constexpr std::size_t DefaultBannerSize = 0x300;
constexpr std::size_t DefaultIconSize = 0x200;
constexpr std::size_t DefaultRomFsSize = 0x2000;
constexpr std::uint8_t LogoSeed = 0x11;
constexpr std::uint8_t CodeSeed = 0x20;
constexpr std::uint8_t BannerSeed = 0x40;
constexpr std::uint8_t IconSeed = 0x60;
constexpr std::uint8_t RomFsSeed = 0x80;

SyntheticImageSpec DefaultImageSpec() {
    SyntheticImageSpec spec;

    spec.logo = PatternBytes(DefaultLogoSize, LogoSeed);
    const std::string plain = "SyntheticLib";
    spec.plain_region.assign(plain.begin(), plain.end());

    // The decompressed .code of a real title equals the page-aligned sum of
    // the three segments exactly, which docs/formats/exefs.md records as the
    // check that validated the decoder. The synthetic image keeps that
    // relationship so a test can rely on it.
    const std::size_t code_size =
        static_cast<std::size_t>(spec.text.pages + spec.read_only.pages + spec.data.pages) *
        domain::ctr::exheader::PageSize;

    spec.exefs_files = {
        ExeFsFileSpec{.name = std::string{domain::ctr::exefs::CodeFileName},
                      .data = PatternBytes(code_size, CodeSeed)},
        ExeFsFileSpec{.name = std::string{domain::ctr::exefs::BannerFileName},
                      .data = PatternBytes(DefaultBannerSize, BannerSeed)},
        ExeFsFileSpec{.name = std::string{domain::ctr::exefs::IconFileName},
                      .data = PatternBytes(DefaultIconSize, IconSeed)},
    };

    spec.romfs = PatternBytes(DefaultRomFsSize, RomFsSeed);
    return spec;
}

SyntheticImage BuildSyntheticImage(const SyntheticImageSpec& spec) {
    const ExeFsLayout exefs = PlanExeFs(spec.exefs_files);
    const PartitionLayout layout = PlanPartition(spec, exefs);

    // Partition 0 starts where the reference image starts it, after the NCSD
    // header and the card info header.
    const std::size_t partition_offset = 0x4000;
    const std::size_t partition_size = AlignUp(layout.total, domain::ctr::MediaUnit);

    std::size_t image_size = partition_offset + partition_size;
    std::vector<std::pair<std::size_t, std::size_t>> extra;
    for (const ExtraPartitionSpec& partition : spec.extra_partitions) {
        const std::size_t size = AlignUp(partition.size, domain::ctr::MediaUnit);
        extra.emplace_back(image_size, size);
        image_size += size;
    }

    SyntheticImage image;
    image.bytes.assign(image_size, PaddingFiller);
    image.partition_offset = partition_offset;
    image.partition_size = partition_size;
    image.exheader_offset = partition_offset + layout.exheader;
    image.logo_offset = layout.logo == 0 ? 0 : partition_offset + layout.logo;
    image.logo_size = layout.logo_size;
    image.plain_offset = layout.plain == 0 ? 0 : partition_offset + layout.plain;
    image.plain_size = layout.plain_size;
    image.exefs_offset = layout.exefs == 0 ? 0 : partition_offset + layout.exefs;
    image.exefs_size = layout.exefs_size;
    image.romfs_offset = layout.romfs == 0 ? 0 : partition_offset + layout.romfs;
    image.romfs_size = layout.romfs_size;

    Placer place{image.bytes};

    WriteNcsd(place, spec, partition_offset, partition_size, image_size);
    WriteNcch(place, partition_offset, spec, layout);
    WriteExHeader(place, image.exheader_offset, spec);
    if (layout.exefs_size > 0) {
        WriteExeFs(place, image.exefs_offset, spec, exefs, image);
    }
    if (layout.logo_size > 0) {
        place.Bytes(image.logo_offset, core::ByteSpan{spec.logo});
    }
    if (layout.plain_size > 0) {
        place.Bytes(image.plain_offset, core::ByteSpan{spec.plain_region});
    }
    if (layout.romfs_size > 0) {
        place.Bytes(image.romfs_offset, core::ByteSpan{spec.romfs});
    }

    for (const std::pair<std::size_t, std::size_t>& partition : extra) {
        place.Fill(partition.first, partition.second, ExtraPartitionFiller);
        place.Text(partition.first + domain::ctr::ncch::MagicOffset,
                   domain::ctr::ncch::Magic,
                   domain::ctr::ncch::Magic.size());
    }
    for (std::size_t index = 0; index < spec.extra_partitions.size(); ++index) {
        const std::size_t entry =
            domain::ctr::ncsd::PartitionTableOffset +
            (spec.extra_partitions[index].index * domain::ctr::ncsd::PartitionTableEntrySize);
        place.U32(entry, InMediaUnits(extra[index].first));
        place.U32(entry + 4, InMediaUnits(extra[index].second));
        place.U8(domain::ctr::ncsd::PartitionFsTypesOffset + spec.extra_partitions[index].index, 1);
    }

    // Hashes last, since each covers bytes the steps above wrote.
    place.Digest(partition_offset + domain::ctr::ncch::ExHeaderHashOffset,
                 image.Span().subspan(image.exheader_offset, domain::ctr::exheader::HashedSize));
    place.Digest(domain::ctr::ncsd::ExHeaderHashOffset,
                 image.Span().subspan(image.exheader_offset, domain::ctr::exheader::HashedSize));
    if (layout.logo_size > 0) {
        place.Digest(partition_offset + domain::ctr::ncch::LogoHashOffset,
                     image.Span().subspan(image.logo_offset, image.logo_size));
    }
    if (layout.exefs_size > 0) {
        place.Digest(partition_offset + domain::ctr::ncch::ExeFsSuperblockHashOffset,
                     image.Span().subspan(image.exefs_offset, domain::ctr::MediaUnit));
    }
    if (layout.romfs_size > 0) {
        place.Digest(partition_offset + domain::ctr::ncch::RomFsSuperblockHashOffset,
                     image.Span().subspan(image.romfs_offset, domain::ctr::MediaUnit));
    }

    return image;
}

}  // namespace hexlith::test
