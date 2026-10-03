#ifndef HEXLITH_DOMAIN_CTR_ROMFS_H
#define HEXLITH_DOMAIN_CTR_ROMFS_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/layout.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::ctr {

struct RomFsFile {
    // Relative, with forward slashes and no leading slash, as the extractor
    // writes it and as the manifest records it.
    std::string path;

    // Relative to the start of the RomFS region, so that reading a file needs
    // nothing but the region and this entry.
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct RomFs {
    // Both lists are sorted by path. The tree walk is already deterministic,
    // but the order a manifest records is worth imposing rather than
    // inheriting, since two runs having to agree byte for byte is the whole
    // point of the manifest.
    std::vector<std::string> directories;
    std::vector<RomFsFile> files;

    std::size_t level3_offset = 0;
    std::size_t file_data_offset = 0;

    [[nodiscard]] const RomFsFile* Find(std::string_view path) const;

    [[nodiscard]] std::size_t TotalFileBytes() const;
};

// Walks the directory tree and returns every file in it.
//
// The hash tables are not read. They exist so the console can look a path up
// without walking the tree, and an extractor that reads every entry once
// gains nothing from them.
//
// A filesystem image is untrusted input, so a name that could escape the
// output directory is refused rather than sanitised. Sanitising would mean
// two different names collapsing to the same file.
core::Result<RomFs> ParseRomFs(core::ByteSpan region);

core::Result<core::ByteSpan> RomFsFileContents(core::ByteSpan region, const RomFsFile& file);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_ROMFS_H
