#include "domain/ctr/backwards_lzss.h"

#include "domain/ctr/layout.h"

#include <format>

namespace hexlith::domain::ctr {
namespace {

// A control byte carries eight flags, consumed from the most significant bit.
constexpr unsigned FlagsPerControlByte = 8;
constexpr std::uint8_t TopFlagMask = 0x80;

// A back-reference is two bytes: the top nibble plus three is the run length,
// and the low twelve bits plus two is how far ahead in the already-written
// output to read from.
constexpr std::size_t BackReferenceSize = 2;
constexpr std::uint16_t RunLengthShift = 12;
constexpr std::uint16_t RunLengthMask = 0x000F;
constexpr std::size_t MinimumRunLength = 3;
constexpr std::uint16_t DistanceMask = 0x0FFF;
constexpr std::size_t DistanceBias = 2;

// One back-reference: two bytes describing a run length and how far ahead in
// the already-written output to read from. The source descends in step with
// the destination, which is what lets a run reproduce a pattern longer than
// itself.
core::Status CopyBackReference(core::ByteSpan compressed,
                               std::vector<std::uint8_t>& output,
                               std::size_t& read,
                               std::size_t stop,
                               std::size_t& write) {
    if (read - stop < BackReferenceSize) {
        return core::Error{std::format(
            "a back-reference at {:#x} needs two bytes and the encoded region ends at {:#x}",
            read,
            stop)};
    }
    read -= BackReferenceSize;

    const auto reference = static_cast<std::uint16_t>(
        compressed[read] | (static_cast<std::uint16_t>(compressed[read + 1]) << 8U));
    const std::size_t run = ((reference >> RunLengthShift) & RunLengthMask) + MinimumRunLength;
    const std::size_t distance = (reference & DistanceMask) + DistanceBias;

    if (write < run) {
        return core::Error{
            std::format("a run of {} bytes would write below the start of a {:#x} byte output",
                        run,
                        output.size())};
    }
    if (write + distance >= output.size()) {
        return core::Error{std::format(
            "a back-reference at output position {:#x} reads {:#x} bytes ahead, outside an "
            "output of {:#x} bytes",
            write,
            distance,
            output.size())};
    }

    for (std::size_t copied = 0; copied < run; ++copied) {
        const std::uint8_t byte = output[write + distance];
        output[--write] = byte;
    }
    return {};
}

core::Status CopyLiteral(core::ByteSpan compressed,
                         std::vector<std::uint8_t>& output,
                         std::size_t& read,
                         std::size_t& write) {
    if (write == 0) {
        return core::Error{std::format(
            "a literal would write below the start of a {:#x} byte output", output.size())};
    }
    output[--write] = compressed[--read];
    return {};
}

}  // namespace

core::Result<BackwardsLzssFooter> ReadBackwardsLzssFooter(core::ByteSpan compressed) {
    if (compressed.size() < exefs::CompressionFooterSize) {
        return core::Error{
            std::format("a compressed section carries an {:#x} byte footer and this one is {:#x}",
                        exefs::CompressionFooterSize,
                        compressed.size())};
    }

    const std::size_t footer_at = compressed.size() - exefs::CompressionFooterSize;
    const std::uint32_t packed = core::ReadU32At(compressed, footer_at).Value();
    const std::uint32_t additional = core::ReadU32At(compressed, footer_at + 4).Value();

    BackwardsLzssFooter footer;
    footer.footer_size = packed >> exefs::FooterSizeShift;
    footer.encoded_size = packed & exefs::EncodedSizeMask;
    footer.additional_size = additional;
    footer.decompressed_size = compressed.size() + additional;

    // The read cursor starts footer_size below the end and stops
    // encoded_size below it, so neither can describe a position outside the
    // input, and the cursor cannot start below where it has to stop.
    if (footer.footer_size > compressed.size()) {
        return core::Error{
            std::format("the footer declares itself {:#x} bytes inside a section of {:#x}",
                        footer.footer_size,
                        compressed.size())};
    }
    if (footer.encoded_size > compressed.size()) {
        return core::Error{
            std::format("the footer declares {:#x} encoded bytes inside a section of {:#x}",
                        footer.encoded_size,
                        compressed.size())};
    }
    if (footer.footer_size > footer.encoded_size) {
        return core::Error{std::format(
            "the footer is {:#x} bytes and the encoded region {:#x}, so decoding would start "
            "below where it has to stop",
            footer.footer_size,
            footer.encoded_size)};
    }

    return footer;
}

core::Result<std::vector<std::uint8_t>> DecompressBackwardsLzss(core::ByteSpan compressed) {
    const core::Result<BackwardsLzssFooter> footer = ReadBackwardsLzssFooter(compressed);
    if (!footer) {
        return footer.Failure();
    }

    const std::size_t decompressed_size = footer.Value().decompressed_size;

    // The output buffer starts as a copy of the input, because the encoding
    // is designed to be decompressed in place and the bottom of the output is
    // never encoded.
    std::vector<std::uint8_t> output(decompressed_size, 0);
    for (std::size_t index = 0; index < compressed.size(); ++index) {
        output[index] = compressed[index];
    }

    std::size_t read = compressed.size() - footer.Value().footer_size;
    const std::size_t stop = compressed.size() - footer.Value().encoded_size;
    std::size_t write = decompressed_size;

    while (read > stop) {
        std::uint8_t control = compressed[--read];

        for (unsigned flag = 0; flag < FlagsPerControlByte; ++flag) {
            // Decoding stops where the encoded region ends, which is not
            // necessarily where the output fills up.
            if (read <= stop) {
                return output;
            }

            const core::Status copied =
                (control & TopFlagMask) != 0
                    ? CopyBackReference(compressed, output, read, stop, write)
                    : CopyLiteral(compressed, output, read, write);
            if (!copied) {
                return copied.Failure();
            }

            control = static_cast<std::uint8_t>(control << 1U);
        }
    }

    return output;
}

}  // namespace hexlith::domain::ctr
