#ifndef HEXLITH_DOMAIN_CTR_EXEFS_H
#define HEXLITH_DOMAIN_CTR_EXEFS_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "core/sha256.h"
#include "domain/ctr/layout.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::ctr {

struct ExeFsFile {
    std::size_t index = 0;
    std::string name;

    // Relative to the end of the ExeFS header, which is where the data area
    // begins.
    std::size_t offset = 0;
    std::size_t size = 0;

    core::Digest hash;
};

struct ExeFs {
    std::vector<ExeFsFile> files;

    [[nodiscard]] const ExeFsFile* Find(std::string_view name) const;
};

// Reads the ExeFS header.
//
// All ten entries the format allows are read. Azahar's extractor, and the
// script derived from it, iterate eight. That is harmless in practice
// because .code is always the first entry, but it is wrong against the
// format and there is no reason to inherit it.
core::Result<ExeFs> ParseExeFs(core::ByteSpan region);

core::Result<core::ByteSpan> ExeFsFileContents(core::ByteSpan region, const ExeFsFile& file);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_EXEFS_H
