#include "support/crafted_lzss_stream.h"

#include <string_view>

namespace hexlith::test {
namespace {

constexpr std::uint32_t ByteMask = 0xFF;
constexpr std::string_view VerbatimPrefix = "VERBATIM";
constexpr std::string_view LiteralText = "ABCDEFGH";

void AppendLittleEndian(std::vector<std::uint8_t>& stream, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        stream.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & ByteMask));
    }
}

void AppendText(std::vector<std::uint8_t>& stream, std::string_view text) {
    for (const char character : text) {
        stream.push_back(static_cast<std::uint8_t>(character));
    }
}

}  // namespace

void AppendLzssFooter(std::vector<std::uint8_t>& stream,
                      std::size_t encoded_size,
                      std::size_t additional_size) {
    AppendLittleEndian(
        stream, static_cast<std::uint32_t>((LzssFooterSize << LzssFooterSizeShift) | encoded_size));
    AppendLittleEndian(stream, static_cast<std::uint32_t>(additional_size));
}

CraftedLzssStream LiteralsOnlyLzssStream() {
    CraftedLzssStream crafted;

    AppendText(crafted.compressed, LiteralText);
    crafted.compressed.push_back(ControlAllLiterals);
    AppendLzssFooter(crafted.compressed, crafted.compressed.size() + LzssFooterSize, 0);

    AppendText(crafted.expected, LiteralText);
    crafted.expected.push_back(ControlAllLiterals);
    AppendText(crafted.expected, LiteralText);
    return crafted;
}

CraftedLzssStream RepeatedByteLzssStream() {
    CraftedLzssStream crafted;

    AppendText(crafted.compressed, VerbatimPrefix);
    const std::size_t encoded_from = crafted.compressed.size();

    for (unsigned reference = 0; reference < FlagsPerControlByte; ++reference) {
        crafted.compressed.push_back(LongestRunLowByte);
        crafted.compressed.push_back(LongestRunHighByte);
    }
    crafted.compressed.push_back(ControlAllReferences);

    for (unsigned literal = 0; literal < FlagsPerControlByte; ++literal) {
        crafted.compressed.push_back(RepeatedByte);
    }
    crafted.compressed.push_back(ControlAllLiterals);

    const std::size_t produced = FlagsPerControlByte + (FlagsPerControlByte * LongestRun);
    const std::size_t decompressed_size = encoded_from + produced;
    const std::size_t total = crafted.compressed.size() + LzssFooterSize;
    AppendLzssFooter(crafted.compressed, total - encoded_from, decompressed_size - total);

    AppendText(crafted.expected, VerbatimPrefix);
    crafted.expected.insert(crafted.expected.end(), produced, RepeatedByte);
    return crafted;
}

}  // namespace hexlith::test
