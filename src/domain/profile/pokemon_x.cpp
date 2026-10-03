#include "domain/profile/pokemon_x.h"

#include <array>

// Every value in this file was measured from the reference cartridge image by
// the container parsers in src/domain/ctr and recorded in docs/formats/. None
// of it is content from the game: they are the program's identity, the
// addresses its code loads at, and hashes of what it contains.
namespace hexlith::domain::profile {
namespace {

// The service access control list, in the order the extended header states
// it. The order is part of what is being checked: a list with the same 28
// entries in a different order is a different title's.
constexpr std::array<std::string_view, 28> Services = {
    "APT:U",    "$hioFIO",  "$hostio0", "$hostio1", "ac:u",     "boss:U", "cam:u",
    "cecd:u",   "cfg:u",    "dlp:FKCL", "dlp:SRVR", "dsp::DSP", "frd:u",  "fs:USER",
    "gsp::Gpu", "hid:USER", "http:C",   "mic:u",    "ndm:u",    "news:u", "nwm::UDS",
    "ptm:u",    "pxi:dev",  "soc:U",    "ssl:C",    "y2r:u",    "ldr:ro", "ir:USER",
};

// The three code sets, from the extended header's system control info.
constexpr SegmentLayout Text{.address = 0x00100000, .page_count = 1084, .size = 4439772};
constexpr SegmentLayout ReadOnly{.address = 0x0053C000, .page_count = 106, .size = 432708};
constexpr SegmentLayout Data{.address = 0x005A6000, .page_count = 69, .size = 278720};

// The sum of the three page-aligned segments, which is what a decompressed
// .code section measures exactly.
constexpr std::size_t DecompressedCodeSize = 5156864;

constexpr GameProfile Profile{
    .name = "Pokemon X",
    .program_id = 0x0004000000055D00,
    .product_code = "CTR-P-EKJA",
    .maker_code = 0x3130,  // "01"
    .ncch_format_version = 2,
    .remaster_version = 0,
    .application_title = "kujira-1",
    .executable_partition = 0,
    .text = Text,
    .read_only = ReadOnly,
    .data = Data,
    .stack_size = 0x40000,
    .bss_size = 0x76858,
    .services = Services,
    .image_digest = "6ac1cdfaad58527b63f540596d7692fe2df91079078852969cfba43faf78b5b7",
    .exheader_digest = "ec2dec5b853d3543c015075a89b3ecd8817d7408a03d8ff30b4dd13813160a18",
    .code_digest = "0f6d3e1d66cbcd55d46fa453db4b6aa489e03c3537c6b24026f2a53af142797a",
    .code_size = DecompressedCodeSize,
    .romfs_file_count = 653,

    // 133 loadable modules plus the static module. The hash container beside
    // them is not one, which is why the extractor decides by the magic at
    // 0x80 rather than by the extension.
    .cro_module_count = 134,
};

}  // namespace

const GameProfile& PokemonX() {
    return Profile;
}

}  // namespace hexlith::domain::profile
