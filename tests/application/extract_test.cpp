#include "application/extract.h"
#include "application/manifest.h"
#include "domain/ctr/exefs.h"
#include "domain/profile/pokemon_x.h"

#include "support/crafted_lzss_stream.h"
#include "support/memory_workspace.h"
#include "support/synthetic_cro.h"
#include "support/synthetic_ctr_image.h"
#include "support/synthetic_romfs.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

using domain::profile::PokemonX;
using test::BuildSyntheticCro;
using test::BuildSyntheticRomFs;
using test::DefaultCroSpec;
using test::DefaultImageSpec;
using test::MemoryBlobSink;
using test::MemoryRomSource;
using test::SyntheticImageSpec;

// Bytes that are not any container's magic, so that the extractor has to
// refuse them.
constexpr std::size_t NotAnImageSize = 0x400;
constexpr std::uint8_t NotAnImageFiller = 0x5A;

// The hash container that looks like a module and is not, and an ordinary
// data file, each with its own filler.
constexpr std::size_t HashContainerSize = 0x80;
constexpr std::uint8_t HashContainerSeed = 0x11;
constexpr std::size_t DataFileSize = 0x100;
constexpr std::uint8_t DataFileSeed = 0x22;

// A RomFS holding modules, the static module and the hash container that
// looks like one but is not, so that the extractor's CRO detection has all
// three cases to get right.
test::SyntheticRomFsSpec RomFsWithModules() {
    test::SyntheticRomFsSpec spec;

    test::SyntheticCroSpec field = DefaultCroSpec();
    field.name = "DllField";
    test::SyntheticCroSpec battle = DefaultCroSpec();
    battle.name = "DllBattle";

    spec.files = {
        test::SyntheticRomFsFile{.path = "DllBattle.cro", .data = BuildSyntheticCro(battle)},
        test::SyntheticRomFsFile{.path = "DllField.cro", .data = BuildSyntheticCro(field)},
        test::SyntheticRomFsFile{.path = "static.crs",
                                 .data = BuildSyntheticCro(test::StaticModuleSpec())},
        test::SyntheticRomFsFile{.path = ".crr/static.crr",
                                 .data = test::PatternBytes(HashContainerSize, HashContainerSeed)},
        test::SyntheticRomFsFile{.path = "a/0/0",
                                 .data = test::PatternBytes(DataFileSize, DataFileSeed)},
    };
    spec.empty_directories = {"shaders"};
    return spec;
}

SyntheticImageSpec SpecWithARealRomFs() {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.romfs = BuildSyntheticRomFs(RomFsWithModules());
    return spec;
}

ExtractRequest Request() {
    // A synthetic image cannot carry the cartridge's hashes, so the checks
    // that depend on them are off here and are what the run against the
    // reference image covers.
    return ExtractRequest{.profile = &PokemonX(), .require_profile_match = false};
}

ExtractReport ExtractOrFail(MemoryRomSource& source, MemoryBlobSink& sink) {
    const core::Result<ExtractReport> report = Extract(source, sink, Request());
    EXPECT_TRUE(report.Ok()) << report.Failure().Message();
    return report.Ok() ? report.Value() : ExtractReport{};
}

TEST(Extract, WritesTheLayoutTheWorkspaceExpects) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    ExtractOrFail(source, sink);

    EXPECT_TRUE(sink.Holds("exheader.bin"));
    EXPECT_TRUE(sink.Holds("logo.bin"));
    EXPECT_TRUE(sink.Holds("plain.txt"));
    EXPECT_TRUE(sink.Holds("exefs/.code"));
    EXPECT_TRUE(sink.Holds("exefs/banner"));
    EXPECT_TRUE(sink.Holds("exefs/icon"));
    EXPECT_TRUE(sink.Holds("romfs/DllField.cro"));
    EXPECT_TRUE(sink.Holds("romfs/a/0/0"));
}

TEST(Extract, WritesTheExtendedHeaderRegionAndNotTheDeclaredHalf) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    ExtractOrFail(source, sink);

    EXPECT_EQ(sink.Files().at("exheader.bin").size(), domain::ctr::exheader::SizeOnDisk);
}

TEST(Extract, CreatesTheDirectoriesIncludingTheEmptyOnes) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    ExtractOrFail(source, sink);

    const auto made = [&sink](std::string_view path) {
        return std::ranges::find(sink.Directories(), path) != sink.Directories().end();
    };
    EXPECT_TRUE(made("exefs"));
    EXPECT_TRUE(made("romfs"));
    EXPECT_TRUE(made("romfs/a/0"));
    EXPECT_TRUE(made("romfs/shaders")) << "a directory the image declares empty is still written";
}

TEST(Extract, WritesEveryRomFsFileWithItsContents) {
    const test::SyntheticRomFsSpec romfs = RomFsWithModules();
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.romfs = BuildSyntheticRomFs(romfs);

    MemoryRomSource source{BuildSyntheticImage(spec).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    EXPECT_EQ(report.romfs_file_count, romfs.files.size());
    for (const test::SyntheticRomFsFile& file : romfs.files) {
        const std::string path = "romfs/" + file.path;
        ASSERT_TRUE(sink.Holds(path)) << path;
        EXPECT_EQ(sink.Files().at(path), file.data) << path;
    }
}

TEST(Extract, DecompressesTheCodeSectionWhenTheExtendedHeaderSaysItIsCompressed) {
    const test::CraftedLzssStream crafted = test::RepeatedByteLzssStream();

    SyntheticImageSpec spec = SpecWithARealRomFs();
    spec.compress_exefs_code = true;
    spec.exefs_files[0].data = crafted.compressed;

    // The page counts have to agree with what the section decompresses to,
    // since the extractor refuses a mismatch.
    const std::size_t pages = crafted.expected.size() / domain::ctr::exheader::PageSize;
    spec.text.pages = static_cast<std::uint32_t>(pages == 0 ? 1 : pages);
    spec.read_only.pages = 0;
    spec.data.pages = 0;
    spec.text.size = static_cast<std::uint32_t>(crafted.expected.size());
    spec.read_only.size = 0;
    spec.data.size = 0;

    MemoryRomSource source{BuildSyntheticImage(spec).bytes};
    MemoryBlobSink sink;
    const core::Result<ExtractReport> report = Extract(source, sink, Request());

    // The crafted stream decodes to 160 bytes, which is not a whole number
    // of pages, so the extractor is right to refuse it. What is being
    // checked is that it refuses for that reason and names both numbers.
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("page-aligned segments sum to"), std::string::npos)
        << report.Failure().Message();
}

TEST(Extract, WritesTheCodeSectionVerbatimWhenItIsNotCompressed) {
    const SyntheticImageSpec spec = SpecWithARealRomFs();
    MemoryRomSource source{BuildSyntheticImage(spec).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    EXPECT_EQ(sink.Files().at("exefs/.code"), spec.exefs_files[0].data);

    const auto code = std::ranges::find_if(
        report.files, [](const ExtractedFile& file) { return file.path == "exefs/.code"; });
    ASSERT_NE(code, report.files.end());
    EXPECT_FALSE(code->decompressed);
    EXPECT_EQ(code->source, ContentSource::ExeFs);
}

// A module is one because of the magic at 0x80, not because of its
// extension. static.crs is one and .crr/static.crr is not.
TEST(Extract, FindsTheModulesByTheirMagicRatherThanTheirNames) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    ASSERT_EQ(report.modules.size(), 3U);

    const auto named = [&report](std::string_view path) {
        return std::ranges::find_if(report.modules, [path](const ExtractedModule& module) {
                   return module.path == path;
               }) != report.modules.end();
    };
    EXPECT_TRUE(named("DllField.cro"));
    EXPECT_TRUE(named("DllBattle.cro"));
    EXPECT_TRUE(named("static.crs"));
    EXPECT_FALSE(named(".crr/static.crr")) << "the hash container is not a module";
}

TEST(Extract, DoesNotWriteTheModulesASecondTime) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    for (const ExtractedModule& module : report.modules) {
        EXPECT_TRUE(sink.Holds("romfs/" + module.path)) << module.path;
        EXPECT_FALSE(sink.Holds("cro/" + module.path)) << module.path;
        EXPECT_FALSE(sink.Holds("modules/" + module.path)) << module.path;
    }
}

TEST(Extract, SummarisesEachModuleFromItsHeader) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    const auto field = std::ranges::find_if(report.modules, [](const ExtractedModule& module) {
        return module.path == "DllField.cro";
    });
    ASSERT_NE(field, report.modules.end());
    EXPECT_EQ(field->header.name, "DllField");
    EXPECT_FALSE(field->header.IsStaticModule());
    EXPECT_GT(field->header.counts.internal_relocations, 0U);

    const auto crs = std::ranges::find_if(
        report.modules, [](const ExtractedModule& module) { return module.path == "static.crs"; });
    ASSERT_NE(crs, report.modules.end());
    EXPECT_TRUE(crs->header.IsStaticModule());
}

TEST(Extract, RecordsASha256ForEveryFileItWrote) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    ASSERT_EQ(report.files.size(), sink.Files().size());
    for (const ExtractedFile& file : report.files) {
        ASSERT_TRUE(sink.Holds(file.path)) << file.path;
        const std::vector<std::uint8_t>& written = sink.Files().at(file.path);
        EXPECT_EQ(file.size, written.size()) << file.path;
        EXPECT_EQ(file.digest, core::Sha256::Of(core::ByteSpan{written})) << file.path;
    }
}

TEST(Extract, SortsWhatItReportsByPath) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    const ExtractReport report = ExtractOrFail(source, sink);

    EXPECT_TRUE(std::ranges::is_sorted(report.files,
                                       [](const ExtractedFile& left, const ExtractedFile& right) {
                                           return left.path < right.path;
                                       }));
    EXPECT_TRUE(std::ranges::is_sorted(
        report.modules, [](const ExtractedModule& left, const ExtractedModule& right) {
            return left.path < right.path;
        }));
}

TEST(Extract, ProducesTheSameManifestTwice) {
    const std::vector<std::uint8_t> image = BuildSyntheticImage(SpecWithARealRomFs()).bytes;

    MemoryRomSource first_source{image};
    MemoryBlobSink first_sink;
    const ExtractReport first = ExtractOrFail(first_source, first_sink);

    MemoryRomSource second_source{image};
    MemoryBlobSink second_sink;
    const ExtractReport second = ExtractOrFail(second_source, second_sink);

    EXPECT_EQ(RenderManifest(first), RenderManifest(second));
    EXPECT_EQ(RenderModules(first), RenderModules(second));
    EXPECT_EQ(first_sink.Files(), second_sink.Files());
}

TEST(Extract, RefusesToRunWithoutAProfile) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;

    const core::Result<ExtractReport> report = Extract(source, sink, ExtractRequest{});
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("never extracted on a guess"), std::string::npos);
    EXPECT_TRUE(sink.Files().empty());
}

// Nothing is written before validation passes, so a refused image leaves the
// work directory as it was.
TEST(Extract, WritesNothingWhenTheImageDoesNotMatchTheProfile) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;

    const core::Result<ExtractReport> report = Extract(
        source, sink, ExtractRequest{.profile = &PokemonX(), .require_profile_match = true});
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("does not match the profile"), std::string::npos)
        << report.Failure().Message();
    EXPECT_TRUE(sink.Files().empty());
    EXPECT_TRUE(sink.Directories().empty());
}

TEST(Extract, ReportsEveryDisagreementWhenItRefusesAnImage) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;

    const core::Result<ExtractReport> report = Extract(
        source, sink, ExtractRequest{.profile = &PokemonX(), .require_profile_match = true});
    ASSERT_FALSE(report.Ok());

    // The default image differs from the profile in several ways at once, and
    // the message carries one line for each.
    EXPECT_GE(std::ranges::count(report.Failure().Message(), '\n'), 3);
}

TEST(Extract, ReportsAnUnreadableImage) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    source.FailWith("the file disappeared");
    MemoryBlobSink sink;

    const core::Result<ExtractReport> report = Extract(source, sink, Request());
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("the file disappeared"), std::string::npos);
}

TEST(Extract, ReportsSomethingThatIsNotACartridgeImage) {
    MemoryRomSource source{std::vector<std::uint8_t>(NotAnImageSize, NotAnImageFiller)};
    MemoryBlobSink sink;

    const core::Result<ExtractReport> report = Extract(source, sink, Request());
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("the cartridge image"), std::string::npos)
        << report.Failure().Message();
}

TEST(Extract, ReportsAFailureToWrite) {
    MemoryRomSource source{BuildSyntheticImage(SpecWithARealRomFs()).bytes};
    MemoryBlobSink sink;
    sink.FailOn("romfs/a/0/0");

    const core::Result<ExtractReport> report = Extract(source, sink, Request());
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("was made to fail"), std::string::npos);
}

TEST(Extract, RefusesAPartitionWithNoRomFs) {
    SyntheticImageSpec spec = DefaultImageSpec();
    spec.romfs.clear();
    MemoryRomSource source{BuildSyntheticImage(spec).bytes};
    MemoryBlobSink sink;

    const core::Result<ExtractReport> report = Extract(source, sink, Request());
    ASSERT_FALSE(report.Ok());
    EXPECT_NE(report.Failure().Message().find("declares no RomFS"), std::string::npos);
}

TEST(Describe, NamesEveryContentSource) {
    EXPECT_EQ(Describe(ContentSource::Ncch), "ncch");
    EXPECT_EQ(Describe(ContentSource::ExeFs), "exefs");
    EXPECT_EQ(Describe(ContentSource::RomFs), "romfs");
}

}  // namespace
}  // namespace hexlith::application
