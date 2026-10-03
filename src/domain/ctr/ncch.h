#ifndef HEXLITH_DOMAIN_CTR_NCCH_H
#define HEXLITH_DOMAIN_CTR_NCCH_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "core/sha256.h"
#include "domain/ctr/layout.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hexlith::domain::ctr {

// The eight flag bytes at 0x188, kept as the format stores them so that a
// value Hexlith does not understand can still be reported.
struct NcchFlags {
    std::uint8_t crypto_method = 0;
    std::uint8_t content_platform = 0;
    std::uint8_t content_type = 0;
    std::uint8_t content_unit_size = 0;
    std::uint8_t bitmask = 0;

    [[nodiscard]] bool NoCrypto() const {
        return (bitmask & ncch::NoCryptoFlag) != 0;
    }

    [[nodiscard]] bool FixedCryptoKey() const {
        return (bitmask & ncch::FixedCryptoKeyFlag) != 0;
    }

    [[nodiscard]] bool NoMountRomFs() const {
        return (bitmask & ncch::NoMountRomFsFlag) != 0;
    }

    // Bit 1 of the content type byte. A partition without it holds data
    // only, which is what the electronic manual and the update data are.
    [[nodiscard]] bool Executable() const {
        return (content_type & ncch::ExecutableContentFlag) != 0;
    }

    // Computed as 0x200 << value, per 3dbrew. Hexlith reads it so that an
    // image using anything other than one media unit is reported rather than
    // silently misread.
    [[nodiscard]] std::size_t ContentUnitSize() const {
        return MediaUnit << content_unit_size;
    }
};

// One region of a partition, in bytes and relative to the start of the NCCH.
struct NcchRegion {
    std::size_t offset = 0;
    std::size_t size = 0;

    [[nodiscard]] bool Present() const {
        return size > 0;
    }
};

struct NcchHeader {
    std::uint64_t partition_id = 0;
    std::uint64_t program_id = 0;
    std::uint16_t maker_code = 0;

    // The NCCH format revision, which is 2 for every standard NCCH. It is
    // not a version of the title, and naming it so has caused confusion
    // before: the title version lives in the NCSD card info header and the
    // executable's own revision in the extended header.
    std::uint16_t format_version = 0;

    std::string product_code;
    std::size_t content_size = 0;

    // The size field reports the half its hash covers, which is 0x400. The
    // region present on disk is 0x800, and that is what exheader describes.
    // docs/formats/exheader.md measures both and explains the difference.
    std::size_t declared_exheader_size = 0;
    NcchRegion exheader;

    NcchFlags flags;

    NcchRegion plain;
    NcchRegion logo;
    NcchRegion exefs;
    NcchRegion romfs;

    std::size_t exefs_hash_region_size = 0;
    std::size_t romfs_hash_region_size = 0;

    core::Digest exheader_hash;
    core::Digest logo_hash;
    core::Digest exefs_superblock_hash;
    core::Digest romfs_superblock_hash;
};

// Reads the header of one content partition.
//
// An encrypted partition is refused. Hexlith handles no keys and implements
// no decryption, so this is a deliberate limit on what the project does and
// not a feature that is missing. ADR 0008 states it.
core::Result<NcchHeader> ParseNcchHeader(core::ByteSpan partition);

// The bytes of one region, bounds checked against the partition. The name is
// used in the failure, since "the ExeFS runs past the end" is useful and "a
// region runs past the end" is not.
core::Result<core::ByteSpan>
RegionContents(core::ByteSpan partition, const NcchRegion& region, std::string_view what);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_NCCH_H
