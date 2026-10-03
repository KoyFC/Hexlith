#ifndef HEXLITH_DOMAIN_PROFILE_GAME_PROFILE_H
#define HEXLITH_DOMAIN_PROFILE_GAME_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hexlith::domain::profile {

// Where a segment of the executable loads, as the extended header states it.
struct SegmentLayout {
    std::uint32_t address = 0;
    std::uint32_t page_count = 0;
    std::uint32_t size = 0;
};

// Everything specific to one title and one version of it, so that core logic
// never asks which game is running and asks the profile instead.
//
// ADR 0005 splits this from the bulk analysis tables. What is here is the few
// dozen identity and layout facts, which are structural: a wrong segment
// address breaks everything downstream, so they are compiled in and type
// checked rather than parsed at startup. The hundreds of thousands of symbol
// records live in text files beside them.
//
// These are facts about the game rather than content from it, which is what
// makes them allowed in the repository at all. ADR 0007 draws that line.
struct GameProfile {
    std::string_view name;

    std::uint64_t program_id = 0;
    std::string_view product_code;
    std::uint16_t maker_code = 0;

    // The NCCH format revision, which is 2 for every standard NCCH. It is
    // checked because an image reporting anything else is not a container
    // this project has seen.
    std::uint16_t ncch_format_version = 0;

    // The revision of this build of the executable, and the version half of
    // the profile key. ADR 0005 keys the registry on the program ID and this.
    std::uint16_t remaster_version = 0;

    std::string_view application_title;

    // The partition the profile names, rather than whichever one a scan would
    // find first.
    std::size_t executable_partition = 0;

    SegmentLayout text;
    SegmentLayout read_only;
    SegmentLayout data;
    std::uint32_t stack_size = 0;
    std::uint32_t bss_size = 0;

    // The service access control list, in the order the extended header
    // states it. It is the work list for the HLE layer.
    std::span<const std::string_view> services;

    // Content hashes, as lowercase hexadecimal so that they can be compared
    // against sha256sum by hand.
    std::string_view image_digest;
    std::string_view exheader_digest;

    // The decompressed .code section, which is what the lifter will consume.
    std::string_view code_digest;
    std::size_t code_size = 0;

    // Figures the extractor checks what it produced against. The list of
    // module names is not here: the lifter is what needs it, and a list with
    // no consumer is a list nobody maintains. It arrives with the analysis
    // layer of ADR 0006.
    std::size_t romfs_file_count = 0;
    std::size_t cro_module_count = 0;
};

// The directory a workspace keeps this title's extraction in, as the program
// ID and the remaster version. Nothing else in the project builds this name,
// so an extraction and a later tool cannot disagree about where it went.
std::string WorkspaceDirectoryName(const GameProfile& profile);

}  // namespace hexlith::domain::profile

#endif  // HEXLITH_DOMAIN_PROFILE_GAME_PROFILE_H
