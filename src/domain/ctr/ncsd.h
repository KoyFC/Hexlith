#ifndef HEXLITH_DOMAIN_CTR_NCSD_H
#define HEXLITH_DOMAIN_CTR_NCSD_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/layout.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hexlith::domain::ctr {

// One entry of the NCSD partition table, converted from media units to
// bytes so that nothing downstream has to remember the unit.
struct PartitionEntry {
    std::size_t index = 0;
    std::size_t offset = 0;
    std::size_t size = 0;

    // A partition with a zero offset and a zero length is absent, which is
    // how a retail cartridge declares the slots it does not use.
    [[nodiscard]] bool Present() const {
        return size > 0;
    }
};

// What the outermost container of a cartridge image says about itself.
//
// The partition table is kept whole rather than reduced to the one partition
// a caller wants, because refusing an image whose layout is unexpected means
// being able to describe the layout that was found.
struct NcsdHeader {
    std::uint64_t media_id = 0;
    std::size_t image_size = 0;
    std::array<PartitionEntry, ncsd::PartitionCount> partitions{};
    std::array<std::uint8_t, ncsd::PartitionCount> filesystem_types{};
    std::array<std::uint8_t, ncsd::PartitionCount> crypt_types{};

    // From the card info header, which only a cartridge image carries.
    bool card_info_present = false;
    std::uint16_t title_version = 0;
    std::uint16_t card_revision = 0;
    std::size_t filled_size = 0;

    [[nodiscard]] const PartitionEntry& Partition(std::size_t index) const {
        return partitions.at(index);
    }

    [[nodiscard]] std::size_t PresentPartitionCount() const;
};

// Reads the header and the partition table out of a whole cartridge image.
//
// Every declared partition is checked against the length of the image, so a
// truncated file is reported here rather than as a confusing failure inside
// whichever parser read past the end.
core::Result<NcsdHeader> ParseNcsdHeader(core::ByteSpan image);

// The bytes of one partition.
//
// The index comes from the caller, which in the extractor means from the
// GameProfile. Nothing here scans for the first usable partition: selecting
// the wrong one silently is a worse outcome than refusing to run, which is
// the rule ADR 0005 states.
core::Result<core::ByteSpan>
PartitionContents(core::ByteSpan image, const NcsdHeader& header, std::size_t index);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_NCSD_H
