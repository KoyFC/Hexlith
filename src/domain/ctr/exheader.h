#ifndef HEXLITH_DOMAIN_CTR_EXHEADER_H
#define HEXLITH_DOMAIN_CTR_EXHEADER_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/layout.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hexlith::domain::ctr {

// Where one segment of the executable loads, and how large it is.
//
// These are the addresses lifted code will use, which is why they belong in
// the GameProfile rather than being read from the image every time.
struct CodeSetInfo {
    std::uint32_t address = 0;
    std::uint32_t page_count = 0;
    std::uint32_t size = 0;

    // The segment occupies whole pages in the guest address space even when
    // its contents do not fill them.
    [[nodiscard]] std::size_t PhysicalRegionSize() const {
        return static_cast<std::size_t>(page_count) * exheader::PageSize;
    }

    [[nodiscard]] bool Present() const {
        return page_count > 0;
    }
};

// The extended header: how to load the executable, and what it is allowed to
// do.
struct ExHeader {
    std::string application_title;

    // Bit 0 of the flags byte is the only thing that says the ExeFS .code
    // section is compressed. Nothing in the ExeFS itself indicates it, which
    // is why this field is what the ExeFS reader has to be told.
    bool compress_exefs_code = false;
    bool sd_application = false;

    // The revision of this build of the executable. Hexlith uses it as the
    // version half of a profile key, because it is the number that
    // identifies the code being lifted.
    std::uint16_t remaster_version = 0;

    CodeSetInfo text;
    CodeSetInfo read_only;
    CodeSetInfo data;
    std::uint32_t stack_size = 0;
    std::uint32_t bss_size = 0;

    std::vector<std::uint64_t> dependencies;
    std::uint64_t save_data_size = 0;
    std::uint64_t jump_id = 0;

    // From the access control info, which repeats the program ID so that the
    // kernel can check it against the one in the NCCH header.
    std::uint64_t program_id = 0;
    std::uint32_t core_version = 0;
    std::uint8_t main_thread_priority = 0;

    // The authoritative statement of which Horizon services the title can
    // reach, and therefore the work list for the HLE layer. A service absent
    // from here cannot be reached by the title at all.
    std::vector<std::string> services;
    std::vector<std::string> extended_services;

    // The access descriptor carries a second copy of the access control
    // info, with a wildcard program ID. Reading it is what distinguishes the
    // descriptor's copy from a duplicate of the real one.
    std::uint64_t limitation_program_id = 0;

    // The sum of the three page-aligned segments. A decompressed .code
    // section equals this exactly, which docs/formats/exefs.md records as
    // the check that validated the decoder against the reference image.
    [[nodiscard]] std::size_t DecompressedCodeSize() const {
        return text.PhysicalRegionSize() + read_only.PhysicalRegionSize() +
               data.PhysicalRegionSize();
    }
};

// Reads the extended header out of the 0x800 byte region the NCCH parser
// reports. The region is twice the size the NCCH header's field declares,
// for the reason docs/formats/exheader.md gives.
core::Result<ExHeader> ParseExHeader(core::ByteSpan region);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_EXHEADER_H
