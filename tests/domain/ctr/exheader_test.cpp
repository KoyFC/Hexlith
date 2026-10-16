#include "domain/ctr/exheader.h"

#include "support/synthetic_ctr_image.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {
namespace {

using test::BuildSyntheticImage;
using test::DefaultImageSpec;
using test::PokemonXProgramId;
using test::SyntheticImage;
using test::SyntheticImageSpec;

// The wildcard program ID the access descriptor's copy of the access control
// info carries, observed on the reference image.
constexpr std::uint64_t WildcardProgramId = 0x00040000FFFFFF00;

core::ByteSpan Region(const SyntheticImage& image) {
    return image.Span().subspan(image.exheader_offset, exheader::SizeOnDisk);
}

ExHeader ParseOrFail(const SyntheticImage& image) {
    const core::Result<ExHeader> header = ParseExHeader(Region(image));
    EXPECT_TRUE(header.Ok()) << header.Failure().Message();
    return header.Ok() ? header.Value() : ExHeader{};
}

TEST(ParseExHeader, ReadsTheSystemControlInfo) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.application_title = "kujira-1";
    spec.remaster_version = 3;
    const ExHeader header = ParseOrFail(BuildSyntheticImage(spec));

    EXPECT_EQ(header.application_title, "kujira-1");
    EXPECT_EQ(header.remaster_version, 3U);
    EXPECT_EQ(header.stack_size, test::DefaultStackSize);
    EXPECT_EQ(header.bss_size, test::DefaultBssSize);
}

TEST(ParseExHeader, ReadsTheThreeCodeSets) {
    const ExHeader header = ParseOrFail(BuildSyntheticImage(DefaultImageSpec()));

    EXPECT_EQ(header.text.address, test::DefaultTextAddress);
    EXPECT_EQ(header.text.page_count, test::DefaultTextPages);
    EXPECT_EQ(header.text.size, test::DefaultTextSize);
    EXPECT_EQ(header.read_only.address, test::DefaultReadOnlyAddress);
    EXPECT_EQ(header.data.address, test::DefaultDataAddress);
    EXPECT_TRUE(header.text.Present());
}

TEST(ParseExHeader, ComputesTheDecompressedCodeSizeFromThePageCounts) {
    const SyntheticImageSpec spec = DefaultImageSpec();
    const ExHeader header = ParseOrFail(BuildSyntheticImage(spec));

    const std::size_t pages = spec.text.pages + spec.read_only.pages + spec.data.pages;
    EXPECT_EQ(header.DecompressedCodeSize(), pages * exheader::PageSize);

    // The relationship the decoder was validated against: a decompressed
    // .code section equals the page-aligned segment sum exactly.
    EXPECT_EQ(header.DecompressedCodeSize(), spec.exefs_files[0].data.size());
}

TEST(ParseExHeader, ReadsTheCompressionFlag) {
    SyntheticImageSpec spec = DefaultImageSpec();
    EXPECT_FALSE(ParseOrFail(BuildSyntheticImage(spec)).compress_exefs_code);

    spec.compress_exefs_code = true;
    const ExHeader header = ParseOrFail(BuildSyntheticImage(spec));
    EXPECT_TRUE(header.compress_exefs_code);
    EXPECT_FALSE(header.sd_application);
}

TEST(ParseExHeader, ReadsTheDependencyListAndStopsAtItsTerminator) {
    const ExHeader header = ParseOrFail(BuildSyntheticImage(DefaultImageSpec()));

    // The builder lists two, and the field holds 48 slots.
    EXPECT_EQ(header.dependencies.size(), 2U);
    for (const std::uint64_t title_id : header.dependencies) {
        EXPECT_NE(title_id, 0U);
    }
}

TEST(ParseExHeader, ReadsTheAccessControlInfo) {
    const ExHeader header = ParseOrFail(BuildSyntheticImage(DefaultImageSpec()));

    EXPECT_EQ(header.program_id, PokemonXProgramId);
    EXPECT_EQ(header.jump_id, PokemonXProgramId);
    EXPECT_NE(header.main_thread_priority, 0U);
}

TEST(ParseExHeader, ReadsTheServiceAccessControlList) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.services = {"fs:USER", "ldr:ro", "APT:U", "gsp::Gpu"};
    const ExHeader header = ParseOrFail(BuildSyntheticImage(spec));

    ASSERT_EQ(header.services.size(), 4U);
    EXPECT_EQ(header.services[0], "fs:USER");
    EXPECT_EQ(header.services[1], "ldr:ro");
    EXPECT_EQ(header.services[3], "gsp::Gpu");
    EXPECT_TRUE(header.extended_services.empty());
}

TEST(ParseExHeader, StopsTheServiceListAtItsFirstEmptyEntry) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.services = {"fs:USER"};
    const ExHeader header = ParseOrFail(BuildSyntheticImage(spec));
    EXPECT_EQ(header.services.size(), 1U);
}

TEST(ParseExHeader, ReadsTheLimitationCopysWildcardProgramId) {
    const ExHeader header = ParseOrFail(BuildSyntheticImage(DefaultImageSpec()));

    // This is what identifies the access descriptor's copy of the access
    // control info rather than a duplicate of the real one.
    EXPECT_EQ(header.limitation_program_id, WildcardProgramId);
    EXPECT_NE(header.limitation_program_id, header.program_id);
}

TEST(ParseExHeader, RefusesARegionSmallerThanTheOneOnDisk) {
    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::Result<ExHeader> header =
        ParseExHeader(Region(image).subspan(0, exheader::HashedSize));

    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("occupies 0x800 bytes on disk"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseExHeader, RefusesASegmentThatDoesNotFitInItsPages) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.text.size = static_cast<std::uint32_t>((spec.text.pages * exheader::PageSize) + 1);
    const SyntheticImage image = BuildSyntheticImage(spec);

    const core::Result<ExHeader> header = ParseExHeader(Region(image));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("the text segment declares"), std::string::npos)
        << header.Failure().Message();
}

TEST(ParseExHeader, RefusesAHeaderWithNoTextSegment) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.text = test::CodeSetSpec{};
    const SyntheticImage image = BuildSyntheticImage(spec);

    const core::Result<ExHeader> header = ParseExHeader(Region(image));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("no text segment"), std::string::npos);
}

TEST(ParseExHeader, RefusesAnApplicationTitleThatIsNotText) {
    SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    image.bytes[image.exheader_offset + exheader::ApplicationTitleOffset + 2] = 0x01;

    const core::Result<ExHeader> header = ParseExHeader(Region(image));
    ASSERT_FALSE(header.Ok());
    EXPECT_NE(header.Failure().Message().find("the application title"), std::string::npos)
        << header.Failure().Message();
}

// Writes a little-endian word into a copy of an extended header.
void Put32(std::vector<std::uint8_t>& region, std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
        region.at(offset + byte) = static_cast<std::uint8_t>(value >> (8 * byte));
    }
}

// A copy of a synthetic extended header with the kernel capability
// descriptors and flag0 replaced. The descriptors are laid out the way 3dbrew
// gives them, the type being the run of ones at the top: kernel version 2.35, a
// 512-entry handle table, memory type 1, and one allowed system call in each of
// the first three mask tables. Flag0 says ideal processor 0, affinity 1, memory
// mode 0.
std::vector<std::uint8_t> RegionWithKernelCapabilities() {
    constexpr std::uint32_t KernelVersion = 0xFC000223;
    constexpr std::uint32_t HandleTable = 0xFE000200;
    constexpr std::uint32_t KernelFlags = 0xFF000101;
    constexpr std::uint32_t MaskTable0 = 0xF0000002;  // call 0x01
    constexpr std::uint32_t MaskTable1 = 0xF1000800;  // call 0x18 + 11 = 0x23
    constexpr std::uint32_t MaskTable2 = 0xF2000004;  // call 0x30 + 2 = 0x32
    constexpr std::uint8_t Flag0 = 0x04;

    const SyntheticImage image = BuildSyntheticImage(DefaultImageSpec());
    const core::ByteSpan original = Region(image);
    std::vector<std::uint8_t> region(original.begin(), original.end());
    const std::vector<std::uint32_t> descriptors = {
        MaskTable0, MaskTable1, MaskTable2, KernelFlags, HandleTable, KernelVersion};
    for (std::size_t index = 0; index < exheader::KernelDescriptorCount; ++index) {
        Put32(region,
              exheader::KernelCapabilitiesOffset + (index * 4),
              index < descriptors.size() ? descriptors.at(index) : exheader::UnusedDescriptor);
    }
    region.at(exheader::AciFlag0Offset) = Flag0;
    return region;
}

TEST(ParseExHeader, ReadsTheKernelCapabilities) {
    const core::Result<ExHeader> header = ParseExHeader(RegionWithKernelCapabilities());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();
    const KernelCapabilities& kernel = header.Value().kernel;
    EXPECT_EQ(kernel.kernel_version, 0x0223);
    EXPECT_EQ(kernel.handle_table_size, 0x200U);
    EXPECT_EQ(kernel.memory_type, 1U);
}

TEST(ParseExHeader, ReadsWhichSystemCallsAreAllowed) {
    const core::Result<ExHeader> header = ParseExHeader(RegionWithKernelCapabilities());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();
    const KernelCapabilities& kernel = header.Value().kernel;
    EXPECT_TRUE(kernel.AllowsSystemCall(0x01));
    EXPECT_TRUE(kernel.AllowsSystemCall(0x23));
    EXPECT_TRUE(kernel.AllowsSystemCall(0x32));
    EXPECT_FALSE(kernel.AllowsSystemCall(0x02));
    EXPECT_FALSE(kernel.AllowsSystemCall(0xFF)) << "past the last table";
}

TEST(ParseExHeader, ReadsFlag0) {
    const core::Result<ExHeader> header = ParseExHeader(RegionWithKernelCapabilities());
    ASSERT_TRUE(header.Ok()) << header.Failure().Message();
    EXPECT_EQ(header.Value().ideal_processor, 0U);
    EXPECT_EQ(header.Value().affinity_mask, 1U);
    EXPECT_EQ(header.Value().system_mode, 0U);
}

}  // namespace
}  // namespace hexlith::domain::ctr
