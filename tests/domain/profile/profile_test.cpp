#include "domain/ctr/exheader.h"
#include "domain/ctr/ncch.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/pokemon_x.h"
#include "domain/profile/registry.h"
#include "domain/profile/validation.h"

#include "support/synthetic_ctr_image.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <ranges>
#include <string>
#include <vector>

namespace hexlith::domain::profile {
namespace {

using test::BuildSyntheticImage;
using test::DefaultImageSpec;
using test::SyntheticImage;
using test::SyntheticImageSpec;

constexpr std::uint64_t PokemonYProgramId = 0x0004000000055E00;

// Values a mismatching image reports, picked so that they are visibly not the
// profile's.
constexpr std::uint32_t OtherTextAddress = 0x00200000;
constexpr std::uint32_t OtherStackSize = 0x1000;
constexpr std::size_t SmallCodeSection = 0x1000;

// An image built to match the profile, so that validation has something that
// should pass. The values come from the profile rather than being restated.
SyntheticImageSpec SpecMatching(const GameProfile& profile) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.program_id = profile.program_id;
    spec.partition_id = profile.program_id;
    spec.product_code = std::string{profile.product_code};
    spec.maker_code = profile.maker_code;
    spec.ncch_version = profile.ncch_format_version;
    spec.remaster_version = profile.remaster_version;
    spec.application_title = std::string{profile.application_title};
    spec.text = test::CodeSetSpec{.address = profile.text.address,
                                  .pages = profile.text.page_count,
                                  .size = profile.text.size};
    spec.read_only = test::CodeSetSpec{.address = profile.read_only.address,
                                       .pages = profile.read_only.page_count,
                                       .size = profile.read_only.size};
    spec.data = test::CodeSetSpec{.address = profile.data.address,
                                  .pages = profile.data.page_count,
                                  .size = profile.data.size};
    spec.stack_size = profile.stack_size;
    spec.bss_size = profile.bss_size;
    spec.services.assign(profile.services.begin(), profile.services.end());

    // The real segments are megabytes. A fixture that reserved them would
    // build a 5 MB image for every test, so the .code section is left as the
    // default and the page counts are what validation checks.
    spec.exefs_files[0].data = test::PatternBytes(SmallCodeSection, 0x20);
    return spec;
}

// A synthetic image cannot carry the cartridge's extended header hash: the
// stored hash is of the header that is there, and a fixture's header is its
// own. The digest is substituted from the profile so that everything else can
// be tested, and the digest itself is checked by running the extractor
// against the reference image.
ImageIdentity IdentityFrom(const SyntheticImage& image, const GameProfile& profile) {
    const core::Result<ctr::NcchHeader> ncch = ctr::ParseNcchHeader(image.Partition());
    EXPECT_TRUE(ncch.Ok()) << ncch.Failure().Message();
    const core::Result<ctr::ExHeader> exheader =
        ctr::ParseExHeader(image.Span().subspan(image.exheader_offset, ctr::exheader::SizeOnDisk));
    EXPECT_TRUE(exheader.Ok()) << exheader.Failure().Message();
    if (!ncch.Ok() || !exheader.Ok()) {
        return ImageIdentity{};
    }
    ImageIdentity identity = IdentityOf(ncch.Value(), exheader.Value());
    identity.exheader_digest = core::ParseDigestHex(profile.exheader_digest).Value();
    return identity;
}

TEST(PokemonX, CarriesTheIdentityMeasuredFromTheReferenceImage) {
    const GameProfile& profile = PokemonX();

    EXPECT_EQ(profile.program_id, 0x0004000000055D00U);
    EXPECT_EQ(profile.product_code, "CTR-P-EKJA");
    EXPECT_EQ(profile.application_title, "kujira-1");
    EXPECT_EQ(profile.executable_partition, 0U);
    EXPECT_EQ(profile.remaster_version, 0U);
    EXPECT_EQ(profile.ncch_format_version, 2U);
}

TEST(PokemonX, CarriesTheSegmentLayoutTheLifterWillUse) {
    const GameProfile& profile = PokemonX();

    EXPECT_EQ(profile.text.address, 0x00100000U);
    EXPECT_EQ(profile.text.size, 4439772U);
    EXPECT_EQ(profile.read_only.address, 0x0053C000U);
    EXPECT_EQ(profile.data.address, 0x005A6000U);
}

// The decompressed .code section measures the sum of the three page-aligned
// segments exactly, which docs/formats/exefs.md records as the check that
// validated the decoder. The profile has to agree with its own numbers.
TEST(PokemonX, DeclaresACodeSizeThatAgreesWithItsPageCounts) {
    const GameProfile& profile = PokemonX();
    const std::size_t pages =
        profile.text.page_count + profile.read_only.page_count + profile.data.page_count;

    EXPECT_EQ(profile.code_size, pages * ctr::exheader::PageSize);
    EXPECT_EQ(profile.code_size, 5156864U);
}

TEST(PokemonX, NamesEveryServiceTheTitleCanReach) {
    const GameProfile& profile = PokemonX();

    EXPECT_EQ(profile.services.size(), 28U);
    EXPECT_NE(std::ranges::find(profile.services, "ldr:ro"), profile.services.end())
        << "the service that establishes the title loads CRO modules at runtime";
}

TEST(PokemonX, CarriesDigestsThatParseAsDigests) {
    const GameProfile& profile = PokemonX();

    EXPECT_TRUE(core::ParseDigestHex(profile.image_digest).Ok());
    EXPECT_TRUE(core::ParseDigestHex(profile.exheader_digest).Ok());
    EXPECT_TRUE(core::ParseDigestHex(profile.code_digest).Ok());
}

TEST(WorkspaceDirectoryName, IsTheProgramIdAndTheRemasterVersion) {
    EXPECT_EQ(WorkspaceDirectoryName(PokemonX()), "0004000000055D00-v00");
}

TEST(KnownProfiles, HoldsTheOneTitleTheProjectSupports) {
    ASSERT_EQ(KnownProfiles().size(), 1U);
    EXPECT_EQ(KnownProfiles()[0], &PokemonX());
}

TEST(FindProfile, FindsTheTitleByProgramIdAndVersion) {
    EXPECT_EQ(FindProfile(PokemonX().program_id, PokemonX().remaster_version), &PokemonX());
}

// Selecting the wrong profile silently is a worse outcome than refusing to
// run, which is the rule ADR 0005 states.
TEST(FindProfile, ReportsNothingForATitleTheBuildDoesNotKnow) {
    EXPECT_EQ(FindProfile(PokemonYProgramId, 0), nullptr);
    EXPECT_EQ(FindProfile(PokemonX().program_id, 1), nullptr)
        << "a different revision of the executable is a different profile";
}

TEST(Mismatches, ReportsNoneForAnImageThatMatches) {
    const SyntheticImage image = BuildSyntheticImage(SpecMatching(PokemonX()));
    const std::vector<std::string> mismatches =
        Mismatches(PokemonX(), IdentityFrom(image, PokemonX()));

    for (const std::string& mismatch : mismatches) {
        ADD_FAILURE() << mismatch;
    }
    EXPECT_TRUE(mismatches.empty());
}

TEST(Mismatches, ReportsAWrongProgramId) {
    SyntheticImageSpec spec = SpecMatching(PokemonX());
    spec.program_id = PokemonYProgramId;
    const std::vector<std::string> mismatches =
        Mismatches(PokemonX(), IdentityFrom(BuildSyntheticImage(spec), PokemonX()));

    ASSERT_EQ(mismatches.size(), 1U);
    EXPECT_NE(mismatches[0].find("the program ID"), std::string::npos) << mismatches[0];
}

TEST(Mismatches, ReportsAWrongSegmentAddressWithBothLayouts) {
    SyntheticImageSpec spec = SpecMatching(PokemonX());
    spec.text.address = OtherTextAddress;
    const std::vector<std::string> mismatches =
        Mismatches(PokemonX(), IdentityFrom(BuildSyntheticImage(spec), PokemonX()));

    ASSERT_EQ(mismatches.size(), 1U);
    EXPECT_NE(mismatches[0].find("the text segment"), std::string::npos) << mismatches[0];
    EXPECT_NE(mismatches[0].find("0x200000"), std::string::npos) << mismatches[0];
}

TEST(Mismatches, ReportsAServiceListOfADifferentLength) {
    SyntheticImageSpec spec = SpecMatching(PokemonX());
    spec.services.pop_back();
    const std::vector<std::string> mismatches =
        Mismatches(PokemonX(), IdentityFrom(BuildSyntheticImage(spec), PokemonX()));

    ASSERT_EQ(mismatches.size(), 1U);
    EXPECT_NE(mismatches[0].find("service access control list"), std::string::npos)
        << mismatches[0];
}

TEST(Mismatches, ReportsAServiceListInADifferentOrder) {
    SyntheticImageSpec spec = SpecMatching(PokemonX());
    std::swap(spec.services[0], spec.services[1]);
    const std::vector<std::string> mismatches =
        Mismatches(PokemonX(), IdentityFrom(BuildSyntheticImage(spec), PokemonX()));

    ASSERT_EQ(mismatches.size(), 2U) << "both positions disagree";
    EXPECT_NE(mismatches[0].find("service 0"), std::string::npos) << mismatches[0];
}

// One mismatch is a different version of the same game. A dozen is a
// different game, and saying so needs all of them rather than the first.
TEST(Mismatches, ReportsEveryDisagreementRatherThanTheFirst) {
    SyntheticImageSpec spec = SpecMatching(PokemonX());
    spec.program_id = PokemonYProgramId;
    spec.product_code = "CTR-P-EK2A";
    spec.application_title = "other";
    spec.stack_size = OtherStackSize;
    spec.text.address = OtherTextAddress;

    const std::vector<std::string> mismatches =
        Mismatches(PokemonX(), IdentityFrom(BuildSyntheticImage(spec), PokemonX()));
    EXPECT_GE(mismatches.size(), 5U);
}

TEST(Mismatches, ReportsAWrongExtendedHeaderHash) {
    // The hash the synthetic image stores is of its own extended header,
    // which is not the one the profile was written against.
    const SyntheticImage image = BuildSyntheticImage(SpecMatching(PokemonX()));
    ImageIdentity identity = IdentityFrom(image, PokemonX());
    identity.exheader_digest = core::Sha256::Of(core::ByteSpan{});

    const std::vector<std::string> mismatches = Mismatches(PokemonX(), identity);
    ASSERT_EQ(mismatches.size(), 1U);
    EXPECT_NE(mismatches[0].find("extended header hash"), std::string::npos) << mismatches[0];
}

}  // namespace
}  // namespace hexlith::domain::profile
