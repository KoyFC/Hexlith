#ifndef HEXLITH_APPLICATION_EXTRACT_H
#define HEXLITH_APPLICATION_EXTRACT_H

#include "core/result.h"
#include "core/sha256.h"
#include "domain/ctr/cro.h"
#include "domain/profile/game_profile.h"
#include "ports/blob_sink.h"
#include "ports/rom_source.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hexlith::application {

// Where a written file came from, which the manifest records so that a
// person reading it does not have to infer the layout.
enum class ContentSource : std::uint8_t { Ncch, ExeFs, RomFs };

std::string_view Describe(ContentSource source);

struct ExtractedFile {
    // Relative to the extraction directory, with forward slashes.
    std::string path;

    std::size_t size = 0;
    core::Digest digest;
    ContentSource source = ContentSource::Ncch;

    // Set on the .code section, whose written bytes are the decompressed
    // ones rather than what the ExeFS held.
    bool decompressed = false;
};

// A CRO-family module found in the RomFS, summarised from its header.
//
// The modules are not written a second time into a directory of their own.
// They already live in the RomFS, extracting the RomFS extracts them, and a
// second copy would be a second thing to keep in step with the first.
struct ExtractedModule {
    std::string path;
    domain::ctr::CroHeader header;
};

struct ExtractReport {
    const domain::profile::GameProfile* profile = nullptr;

    core::Digest image_digest;
    std::size_t image_size = 0;

    std::size_t partition_index = 0;
    std::size_t partition_offset = 0;
    std::size_t partition_size = 0;

    // Sorted by path, so that two runs produce the same manifest.
    std::vector<ExtractedFile> files;
    std::vector<ExtractedModule> modules;

    std::size_t romfs_file_count = 0;
    std::size_t romfs_directory_count = 0;

    [[nodiscard]] std::size_t TotalWrittenBytes() const;
};

struct ExtractRequest {
    // The profile the image is validated against. Required: an image is
    // never extracted on a guess.
    const domain::profile::GameProfile* profile = nullptr;

    // Off only for tests, which work against synthetic images that cannot
    // carry the cartridge's hashes. A tool always leaves it on.
    bool require_profile_match = true;
};

// Validates an image against a profile and writes its contents out.
//
// Nothing is written before validation passes, so a refused image leaves the
// work directory as it was.
core::Result<ExtractReport>
Extract(ports::RomSource& source, ports::BlobSink& sink, const ExtractRequest& request);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_EXTRACT_H
