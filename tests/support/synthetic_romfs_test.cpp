#include "core/byte_reader.h"
#include "domain/ctr/layout.h"

#include "support/synthetic_romfs.h"

#include <cstddef>
#include <gtest/gtest.h>

// The builder's own shape, asserted against the literals from
// docs/formats/romfs.md rather than against the constants the parser shares
// with it.
namespace hexlith::test {
namespace {

TEST(BuildSyntheticRomFs, PutsTheIvfcMagicAtTheStart) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());
    EXPECT_TRUE(core::MatchesAt(core::ByteSpan{region}, 0x00, "IVFC"));
    EXPECT_EQ(core::ReadU32At(core::ByteSpan{region}, 0x04).Value(), 0x10000U);
}

TEST(BuildSyntheticRomFs, PutsLevel3AtOneThousandSixtyFourBytesIn) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());

    // 0x1000 into the region, with a header length field of 0x28.
    EXPECT_EQ(core::ReadU32At(core::ByteSpan{region}, 0x1000).Value(), 0x28U);
}

TEST(BuildSyntheticRomFs, WritesALevel3LogicalOffsetThatIsNotAFileOffset) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());

    // The reference image reports 0xD66000 here, where nothing parses. A
    // parser that used it would fail against this fixture too, which is the
    // point of writing it.
    const std::size_t logical =
        static_cast<std::size_t>(core::ReadU64At(core::ByteSpan{region}, 0x3C).Value());
    EXPECT_EQ(logical, 0xD66000U);
    EXPECT_NE(logical, domain::ctr::romfs::Level3Offset);
}

TEST(BuildSyntheticRomFs, FillsTheHashBucketsWithTheEmptyMarker) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());
    const std::size_t level3 = 0x1000;
    const std::size_t hash_at =
        level3 + core::ReadU32At(core::ByteSpan{region}, level3 + 0x04).Value();

    EXPECT_EQ(core::ReadU32At(core::ByteSpan{region}, hash_at).Value(), 0xFFFFFFFFU);
}

TEST(BuildSyntheticRomFs, TerminatesTheRootsSiblingChain) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(DefaultRomFsSpec());
    const std::size_t level3 = 0x1000;
    const std::size_t directories =
        level3 + core::ReadU32At(core::ByteSpan{region}, level3 + 0x0C).Value();

    // The root points at itself as its parent and has no sibling.
    EXPECT_EQ(core::ReadU32At(core::ByteSpan{region}, directories + 0x00).Value(), 0U);
    EXPECT_EQ(core::ReadU32At(core::ByteSpan{region}, directories + 0x04).Value(), 0xFFFFFFFFU);
    EXPECT_EQ(core::ReadU32At(core::ByteSpan{region}, directories + 0x14).Value(), 0U)
        << "the root has no name";
}

TEST(BuildSyntheticRomFs, IsDeterministic) {
    EXPECT_EQ(BuildSyntheticRomFs(DefaultRomFsSpec()), BuildSyntheticRomFs(DefaultRomFsSpec()));
}

TEST(BuildSyntheticRomFs, BuildsAnEmptyFilesystem) {
    const std::vector<std::uint8_t> region = BuildSyntheticRomFs(SyntheticRomFsSpec{});
    EXPECT_GT(region.size(), 0x1000U);
    EXPECT_TRUE(core::MatchesAt(core::ByteSpan{region}, 0x00, "IVFC"));
}

}  // namespace
}  // namespace hexlith::test
