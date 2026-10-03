#include "application/manifest.h"
#include "domain/profile/pokemon_x.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

// Figures measured from the reference image, so that the rendered manifest in
// a test reads like the one a real run produces.
constexpr std::size_t ImageSize = 0x80000000;
constexpr std::size_t PartitionOffset = 0x4000;
constexpr std::size_t PartitionSize = 0x6B59E000;
constexpr std::size_t DecompressedCodeSize = 0x4EB000;
constexpr std::size_t ExHeaderSizeOnDisk = 0x800;
constexpr std::size_t DllFieldCodeOffset = 0x180;
constexpr std::size_t DllFieldCodeSize = 0xECF64;
constexpr std::size_t DllFieldInternalRelocations = 10237;
constexpr std::size_t DllFieldNamedExports = 2;

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::vector<std::string> RecordsOfKind(const std::string& text, std::string_view kind) {
    std::vector<std::string> records;
    for (const std::string& line : Lines(text)) {
        if (line.starts_with(std::string{kind} + " ")) {
            records.push_back(line);
        }
    }
    return records;
}

ExtractReport SmallReport() {
    ExtractReport report;
    report.profile = &domain::profile::PokemonX();
    report.image_size = ImageSize;
    report.image_digest = core::ParseDigestHex(report.profile->image_digest).Value();
    report.partition_index = 0;
    report.partition_offset = PartitionOffset;
    report.partition_size = PartitionSize;
    report.romfs_file_count = 2;
    report.romfs_directory_count = 1;

    report.files = {
        ExtractedFile{.path = "exefs/.code",
                      .size = DecompressedCodeSize,
                      .digest = core::Sha256::Of(core::ByteSpan{}),
                      .source = ContentSource::ExeFs,
                      .decompressed = true},
        ExtractedFile{.path = "exheader.bin",
                      .size = ExHeaderSizeOnDisk,
                      .digest = core::Sha256::Of(core::ByteSpan{}),
                      .source = ContentSource::Ncch,
                      .decompressed = false},
    };

    domain::ctr::CroHeader header;
    header.name = "DllField";
    header.segments = {domain::ctr::CroSegment{.index = 0,
                                               .offset = DllFieldCodeOffset,
                                               .size = DllFieldCodeSize,
                                               .raw_type = 0,
                                               .type = domain::ctr::CroSegmentType::Code,
                                               .type_known = true}};
    header.counts.internal_relocations = DllFieldInternalRelocations;
    header.counts.named_exports = DllFieldNamedExports;
    report.modules = {ExtractedModule{.path = "DllField.cro", .header = header}};

    return report;
}

TEST(RenderManifest, StartsWithTheFormatVersion) {
    const std::vector<std::string> records =
        RecordsOfKind(RenderManifest(SmallReport()), "manifest-version");
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0], "manifest-version value=1");
}

TEST(RenderManifest, NamesTheProfileItValidatedAgainst) {
    const std::vector<std::string> records =
        RecordsOfKind(RenderManifest(SmallReport()), "profile");
    ASSERT_EQ(records.size(), 1U);
    EXPECT_NE(records[0].find("program-id=0004000000055D00"), std::string::npos) << records[0];
    EXPECT_NE(records[0].find("remaster-version=00"), std::string::npos) << records[0];
}

TEST(RenderManifest, RecordsTheImageItReadAndThePartitionItTook) {
    const std::string text = RenderManifest(SmallReport());

    EXPECT_NE(text.find("image size=0x80000000"), std::string::npos);
    EXPECT_NE(text.find(domain::profile::PokemonX().image_digest), std::string::npos);
    EXPECT_NE(text.find("partition index=0 offset=0x4000 size=0x6b59e000"), std::string::npos)
        << text;
}

TEST(RenderManifest, RecordsOneLinePerFile) {
    const std::vector<std::string> records = RecordsOfKind(RenderManifest(SmallReport()), "file");
    ASSERT_EQ(records.size(), 2U);
    EXPECT_NE(records[0].find("source=exefs"), std::string::npos) << records[0];
    EXPECT_NE(records[0].find("decompressed=yes"), std::string::npos) << records[0];
    EXPECT_NE(records[1].find("decompressed=no"), std::string::npos) << records[1];
}

// Everything after path= is the path, so a name holding a space cannot be
// confused with another field.
TEST(RenderManifest, PutsThePathLastOnEveryRecordThatHasOne) {
    const std::string text = RenderManifest(SmallReport());

    for (const std::string& line : Lines(text)) {
        if (line.starts_with("#")) {
            continue;
        }
        const std::size_t at = line.find("path=");
        if (at == std::string::npos) {
            continue;
        }
        EXPECT_EQ(line.find(' ', at), std::string::npos) << "a field follows the path on: " << line;
    }
}

TEST(RenderManifest, EndsEveryLineAndHoldsNoCarriageReturns) {
    const std::string text = RenderManifest(SmallReport());

    EXPECT_TRUE(text.ends_with("\n"));
    EXPECT_EQ(text.find('\r'), std::string::npos);
}

// Two runs over the same image have to produce the same bytes, which rules
// out anything from the clock or from the host filesystem's ordering.
TEST(RenderManifest, RendersTheSameBytesTwice) {
    EXPECT_EQ(RenderManifest(SmallReport()), RenderManifest(SmallReport()));
}

TEST(RenderManifest, HoldsNothingThatLooksLikeATimeOrAnAbsolutePath) {
    const std::string text = RenderManifest(SmallReport());

    EXPECT_EQ(text.find("/home/"), std::string::npos);
    EXPECT_EQ(text.find("/tmp/"), std::string::npos);
    EXPECT_EQ(text.find("T00:"), std::string::npos) << "an ISO timestamp";
    EXPECT_EQ(text.find("recomp-data"), std::string::npos) << "a path from the host";
}

TEST(RenderModules, RecordsTheHeaderAndTheTablesOfEachModule) {
    const std::string text = RenderModules(SmallReport());

    const std::vector<std::string> modules = RecordsOfKind(text, "module");
    ASSERT_EQ(modules.size(), 1U);
    EXPECT_NE(modules[0].find("name=DllField"), std::string::npos) << modules[0];
    EXPECT_NE(modules[0].find("static=no"), std::string::npos) << modules[0];
    EXPECT_NE(modules[0].find("path=DllField.cro"), std::string::npos) << modules[0];

    const std::vector<std::string> tables = RecordsOfKind(text, "module-tables");
    ASSERT_EQ(tables.size(), 1U);
    EXPECT_NE(tables[0].find("internal-relocations=10237"), std::string::npos) << tables[0];
    EXPECT_NE(tables[0].find("named-exports=2"), std::string::npos) << tables[0];

    const std::vector<std::string> segments = RecordsOfKind(text, "module-segment");
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_NE(segments[0].find("type=code"), std::string::npos) << segments[0];
}

TEST(RenderModules, RendersTheSameBytesTwice) {
    EXPECT_EQ(RenderModules(SmallReport()), RenderModules(SmallReport()));
}

TEST(RenderModules, RendersAnEmptyListWithoutARecord) {
    ExtractReport report = SmallReport();
    report.modules.clear();

    const std::string text = RenderModules(report);
    EXPECT_NE(text.find("modules count=0"), std::string::npos);
    EXPECT_TRUE(RecordsOfKind(text, "module").empty());
}

}  // namespace
}  // namespace hexlith::application
