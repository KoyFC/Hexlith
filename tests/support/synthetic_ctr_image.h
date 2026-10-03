#ifndef HEXLITH_TESTS_SUPPORT_SYNTHETIC_CTR_IMAGE_H
#define HEXLITH_TESTS_SUPPORT_SYNTHETIC_CTR_IMAGE_H

#include "core/byte_reader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Builds CTR container images in memory, so that every parser in the project
// can be tested without a cartridge.
//
// This is the whole reason the test suite runs on a machine that owns no
// game. It is also the reason the tests are worth anything: a synthetic image
// can be made malformed in one specific way, which a real image cannot.
//
// The builder lays regions out the way the formats require, using the same
// field offsets the parsers read, from domain/ctr/layout.h. A test that wants
// a malformed image builds a well-formed one and overwrites a byte at an
// offset the result reports, rather than the builder growing a knob for every
// way an image can be wrong.
namespace hexlith::test {

// The identity of the title Hexlith targets. The builder defaults to it so
// that a test exercising profile validation does not have to restate it, and
// so that a parser reading the wrong field produces a value that is visibly
// not this one.
constexpr std::uint64_t PokemonXProgramId = 0x0004000000055D00;

// "01" as the reference image reports it, read as a little-endian 16-bit
// field.
constexpr std::uint16_t NintendoMakerCode = 0x3130;

// The NCCH format revision, which is 2 for every standard NCCH and is not a
// version of the title.
constexpr std::uint16_t StandardNcchVersion = 2;

// Content type 0x03 is data and executable, which is what the reference
// image reports for its executable partition.
constexpr std::uint8_t DataAndExecutableContent = 0x03;

constexpr std::uint8_t ContentPlatformCtr = 1;

// A small synthetic address map. The text segment starts where the reference
// image starts its own, so that an address in a test looks like an address
// from the title rather than like an arbitrary number, and the three
// segments are a few pages each so an image builds in microseconds.
constexpr std::uint32_t DefaultTextAddress = 0x00100000;
constexpr std::uint32_t DefaultTextPages = 2;
constexpr std::uint32_t DefaultTextSize = 0x1800;
constexpr std::uint32_t DefaultReadOnlyAddress = 0x00102000;
constexpr std::uint32_t DefaultReadOnlyPages = 1;
constexpr std::uint32_t DefaultReadOnlySize = 0x0C00;
constexpr std::uint32_t DefaultDataAddress = 0x00103000;
constexpr std::uint32_t DefaultDataPages = 1;
constexpr std::uint32_t DefaultDataSize = 0x0400;
constexpr std::uint32_t DefaultStackSize = 0x4000;
constexpr std::uint32_t DefaultBssSize = 0x200;

// The electronic manual and the update data, which are the two partitions
// besides the executable one that a retail cartridge carries. Their contents
// are opaque filler: the point of declaring them is that a parser has to
// select the partition the profile names rather than take the first usable
// one.
constexpr std::size_t ManualPartitionIndex = 1;
constexpr std::size_t UpdatePartitionIndex = 7;
constexpr std::size_t DefaultManualPartitionSize = 0x1000;
constexpr std::size_t DefaultUpdatePartitionSize = 0x2000;

// One segment as the extended header describes it.
struct CodeSetSpec {
    std::uint32_t address = 0;
    std::uint32_t pages = 0;
    std::uint32_t size = 0;
};

struct ExeFsFileSpec {
    std::string name;
    std::vector<std::uint8_t> data;
};

// A partition other than the executable one, declared present in the table
// so that a parser has to select rather than assume.
struct ExtraPartitionSpec {
    std::size_t index = 0;
    std::size_t size = 0;
};

// Defaults describe a well-formed image shaped like a retail cartridge, with
// everything small enough to build in a few microseconds. A test changes
// only the field it is about.
struct SyntheticImageSpec {
    std::uint64_t media_id = PokemonXProgramId;
    std::uint64_t program_id = PokemonXProgramId;
    std::uint64_t partition_id = PokemonXProgramId;
    std::string product_code = "CTR-P-TEST";
    std::uint16_t maker_code = NintendoMakerCode;
    std::uint16_t ncch_version = StandardNcchVersion;
    std::uint16_t title_version = 0;
    std::uint8_t content_type = DataAndExecutableContent;
    std::uint8_t content_platform = ContentPlatformCtr;
    bool no_crypto = true;

    std::string application_title = "synthetic";
    std::uint16_t remaster_version = 0;
    bool compress_exefs_code = false;

    CodeSetSpec text{
        .address = DefaultTextAddress, .pages = DefaultTextPages, .size = DefaultTextSize};
    CodeSetSpec read_only{.address = DefaultReadOnlyAddress,
                          .pages = DefaultReadOnlyPages,
                          .size = DefaultReadOnlySize};
    CodeSetSpec data{
        .address = DefaultDataAddress, .pages = DefaultDataPages, .size = DefaultDataSize};
    std::uint32_t stack_size = DefaultStackSize;
    std::uint32_t bss_size = DefaultBssSize;
    std::vector<std::string> services{"fs:USER", "ldr:ro", "APT:U"};

    // An empty region is absent, and its offset and size fields read zero.
    std::vector<std::uint8_t> logo;
    std::vector<std::uint8_t> plain_region;
    std::vector<ExeFsFileSpec> exefs_files;
    std::vector<std::uint8_t> romfs;

    std::vector<ExtraPartitionSpec> extra_partitions{
        ExtraPartitionSpec{.index = ManualPartitionIndex, .size = DefaultManualPartitionSize},
        ExtraPartitionSpec{.index = UpdatePartitionIndex, .size = DefaultUpdatePartitionSize},
    };
};

// Where everything landed. A test asserts against these rather than
// recomputing the layout, except where the point of the test is the layout
// arithmetic itself.
struct SyntheticImage {
    std::vector<std::uint8_t> bytes;

    std::size_t partition_offset = 0;
    std::size_t partition_size = 0;
    std::size_t exheader_offset = 0;
    std::size_t logo_offset = 0;
    std::size_t logo_size = 0;
    std::size_t plain_offset = 0;
    std::size_t plain_size = 0;
    std::size_t exefs_offset = 0;
    std::size_t exefs_size = 0;
    std::size_t romfs_offset = 0;
    std::size_t romfs_size = 0;

    // Absolute offsets of each ExeFS file's data, in the order the spec
    // listed them.
    std::vector<std::size_t> exefs_file_offsets;

    [[nodiscard]] core::ByteSpan Span() const {
        return core::ByteSpan{bytes};
    }

    [[nodiscard]] core::ByteSpan Partition() const {
        return Span().subspan(partition_offset, partition_size);
    }
};

SyntheticImage BuildSyntheticImage(const SyntheticImageSpec& spec);

// A spec whose ExeFS, logo, plain region and RomFS are all populated with
// recognisable content, which is what most tests want.
SyntheticImageSpec DefaultImageSpec();

// Deterministic filler. Content that is distinguishable from zero and from
// the next region, so a parser reading the wrong offset produces a visibly
// wrong answer rather than a plausible one.
std::vector<std::uint8_t> PatternBytes(std::size_t size, std::uint8_t seed);

}  // namespace hexlith::test

#endif  // HEXLITH_TESTS_SUPPORT_SYNTHETIC_CTR_IMAGE_H
