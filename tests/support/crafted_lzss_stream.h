#ifndef HEXLITH_TESTS_SUPPORT_CRAFTED_LZSS_STREAM_H
#define HEXLITH_TESTS_SUPPORT_CRAFTED_LZSS_STREAM_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Compressed ExeFS sections built by hand, together with what they decode to.
//
// There is no encoder in the project, so a compressed section cannot be
// produced by compressing something. These streams are laid out against the
// algorithm docs/formats/exefs.md describes, and their expected output is
// written out rather than computed, so that the decoder is checked against
// the documentation instead of against itself.
//
// They live here rather than in one test because the ExeFS tests need the
// same streams: the extended header's compression flag and the decoder only
// meet in the extractor, and that path is worth testing end to end.
namespace hexlith::test {

// A control byte carries eight flags. These are the two the streams use.
constexpr std::uint8_t ControlAllLiterals = 0x00;
constexpr std::uint8_t ControlAllReferences = 0xFF;
constexpr std::uint8_t ControlOneReference = 0x80;

constexpr std::size_t LzssFooterSize = 8;
constexpr unsigned LzssFooterSizeShift = 24;

// The longest run the encoding expresses, reading from the nearest position
// it allows: a top nibble of 15 means 18 bytes, and a distance field of zero
// means two positions ahead of the write cursor.
constexpr std::uint8_t LongestRunLowByte = 0x00;
constexpr std::uint8_t LongestRunHighByte = 0xF0;
constexpr std::size_t LongestRun = 18;

constexpr unsigned FlagsPerControlByte = 8;

// The byte the compressible stream repeats, and the length of the verbatim
// prefix that decoding never reaches.
constexpr std::uint8_t RepeatedByte = 0xAA;
constexpr std::size_t VerbatimPrefixSize = 8;

struct CraftedLzssStream {
    std::vector<std::uint8_t> compressed;
    std::vector<std::uint8_t> expected;
};

void AppendLzssFooter(std::vector<std::uint8_t>& stream,
                      std::size_t encoded_size,
                      std::size_t additional_size);

// Eight literals and nothing else.
//
// The payload holds the eight bytes in ascending output order followed by
// their control byte, since decoding reads the control first and the
// literals downwards from it. With no additional size the output is as long
// as the input, so the bottom nine bytes are the verbatim copy the format
// leaves there, the control byte included.
CraftedLzssStream LiteralsOnlyLzssStream();

// A verbatim prefix, then eight literals, then eight runs of eighteen bytes
// each copying from what came before.
//
// From the bottom: eight bytes decoding never touches, the group of
// back-references, the group of literals, the footer. Decoding starts at the
// top, so the literals are produced first and every run reads output the
// previous step wrote. Seventeen bytes of references turn into 144 bytes of
// output, which is what makes the stream smaller than what it decodes to and
// so a stream the format can express at all.
CraftedLzssStream RepeatedByteLzssStream();

}  // namespace hexlith::test

#endif  // HEXLITH_TESTS_SUPPORT_CRAFTED_LZSS_STREAM_H
