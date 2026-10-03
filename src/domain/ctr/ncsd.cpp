#include "domain/ctr/ncsd.h"

#include <format>

namespace hexlith::domain::ctr {
namespace {

core::Status
RequireRange(core::ByteSpan image, std::size_t offset, std::size_t size, std::string_view what) {
    if (offset > image.size() || size > image.size() - offset) {
        return core::Error{std::format(
            "{} runs from {:#x} for {:#x} bytes, past the end of an image of {:#x} bytes",
            what,
            offset,
            size,
            image.size())};
    }
    return {};
}

core::Result<PartitionEntry> ReadPartitionEntry(core::ByteSpan image, std::size_t index) {
    const std::size_t entry = ncsd::PartitionTableOffset + (index * ncsd::PartitionTableEntrySize);

    const core::Result<std::uint32_t> offset_units = core::ReadU32At(image, entry);
    if (!offset_units) {
        return offset_units.Failure().WithContext(std::format("partition {} table entry", index));
    }
    const core::Result<std::uint32_t> size_units = core::ReadU32At(image, entry + 4);
    if (!size_units) {
        return size_units.Failure().WithContext(std::format("partition {} table entry", index));
    }

    PartitionEntry partition;
    partition.index = index;
    partition.offset = BytesFromMediaUnits(offset_units.Value());
    partition.size = BytesFromMediaUnits(size_units.Value());

    // A size with no offset, or an offset with no size, is neither a present
    // partition nor an absent one. Guessing which it meant is how an
    // extractor ends up reading from offset zero.
    if ((partition.offset == 0) != (partition.size == 0)) {
        return core::Error{std::format(
            "partition {} declares an offset of {:#x} and a size of {:#x}, so it is neither "
            "present nor absent",
            index,
            partition.offset,
            partition.size)};
    }

    return partition;
}

void ReadCardInfo(core::ByteSpan image, NcsdHeader& header) {
    if (image.size() < ncsd::card_info::EndOffset) {
        return;
    }

    header.card_info_present = true;
    header.title_version = core::ReadU16At(image, ncsd::card_info::TitleVersionOffset).ValueOr(0);
    header.card_revision = core::ReadU16At(image, ncsd::card_info::CardRevisionOffset).ValueOr(0);
    // In bytes, unlike every other size in an NCSD. Measured against the
    // reference image, where the field equals the end of the last partition
    // exactly; read as media units the same value would describe a 939 GB
    // cartridge.
    header.filled_size = core::ReadU32At(image, ncsd::card_info::FilledSizeOffset).ValueOr(0);
}

}  // namespace

std::size_t NcsdHeader::PresentPartitionCount() const {
    std::size_t count = 0;
    for (const PartitionEntry& partition : partitions) {
        if (partition.Present()) {
            ++count;
        }
    }
    return count;
}

core::Result<NcsdHeader> ParseNcsdHeader(core::ByteSpan image) {
    if (image.size() < ncsd::HeaderSize) {
        return core::Error{std::format("an NCSD header is {:#x} bytes and the image is {:#x}",
                                       ncsd::HeaderSize,
                                       image.size())};
    }

    if (!core::MatchesAt(image, ncsd::MagicOffset, ncsd::Magic)) {
        return core::Error{
            std::format("there is no NCSD magic at offset {:#x}, so this is not a cartridge image",
                        ncsd::MagicOffset)};
    }

    NcsdHeader header;
    header.media_id = core::ReadU64At(image, ncsd::MediaIdOffset).Value();
    header.image_size = BytesFromMediaUnits(core::ReadU32At(image, ncsd::ImageSizeOffset).Value());

    if (header.image_size == 0) {
        return core::Error{"the NCSD header declares an image size of zero"};
    }
    if (header.image_size > image.size()) {
        return core::Error{
            std::format("the NCSD header declares {:#x} bytes and the file holds {:#x}, so the "
                        "image is truncated",
                        header.image_size,
                        image.size())};
    }

    for (std::size_t index = 0; index < ncsd::PartitionCount; ++index) {
        header.filesystem_types.at(index) =
            core::ReadU8At(image, ncsd::PartitionFsTypesOffset + index).Value();
        header.crypt_types.at(index) =
            core::ReadU8At(image, ncsd::PartitionCryptTypesOffset + index).Value();

        const core::Result<PartitionEntry> partition = ReadPartitionEntry(image, index);
        if (!partition) {
            return partition.Failure();
        }

        if (partition.Value().Present()) {
            const core::Status within = RequireRange(image,
                                                     partition.Value().offset,
                                                     partition.Value().size,
                                                     std::format("partition {}", index));
            if (!within) {
                return within.Failure();
            }
        }

        header.partitions.at(index) = partition.Value();
    }

    ReadCardInfo(image, header);
    return header;
}

core::Result<core::ByteSpan>
PartitionContents(core::ByteSpan image, const NcsdHeader& header, std::size_t index) {
    if (index >= ncsd::PartitionCount) {
        return core::Error{std::format(
            "partition {} does not exist: an NCSD holds {}", index, ncsd::PartitionCount)};
    }

    const PartitionEntry& partition = header.Partition(index);
    if (!partition.Present()) {
        return core::Error{std::format("partition {} is absent from this image", index)};
    }

    return core::ReadBytesAt(image, partition.offset, partition.size);
}

}  // namespace hexlith::domain::ctr
