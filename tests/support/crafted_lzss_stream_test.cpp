#include "support/crafted_lzss_stream.h"

#include <cstddef>
#include <gtest/gtest.h>

// The crafted streams are the decoder's only reference, so their own shape is
// asserted here. A stream that stopped being what its comment says would
// otherwise turn into a decoder test that passes for the wrong reason.
namespace hexlith::test {
namespace {

TEST(LiteralsOnlyLzssStream, IsAsLongAsWhatItDecodesTo) {
    const CraftedLzssStream crafted = LiteralsOnlyLzssStream();
    EXPECT_EQ(crafted.compressed.size(), crafted.expected.size());
    EXPECT_EQ(crafted.compressed.size(), 17U);
}

TEST(RepeatedByteLzssStream, IsSmallerThanWhatItDecodesTo) {
    const CraftedLzssStream crafted = RepeatedByteLzssStream();
    EXPECT_LT(crafted.compressed.size(), crafted.expected.size());

    // Eight literals and eight runs of eighteen, above an eight byte
    // verbatim prefix.
    EXPECT_EQ(crafted.expected.size(), VerbatimPrefixSize + 8 + (8 * LongestRun));
}

TEST(RepeatedByteLzssStream, KeepsItsVerbatimPrefixInBothHalves) {
    const CraftedLzssStream crafted = RepeatedByteLzssStream();
    for (std::size_t index = 0; index < VerbatimPrefixSize; ++index) {
        EXPECT_EQ(crafted.compressed[index], crafted.expected[index]) << index;
    }
    EXPECT_EQ(crafted.expected[VerbatimPrefixSize], RepeatedByte);
}

}  // namespace
}  // namespace hexlith::test
