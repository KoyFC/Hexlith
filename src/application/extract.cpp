#include "application/extract.h"

#include "domain/ctr/backwards_lzss.h"
#include "domain/ctr/exefs.h"
#include "domain/ctr/exheader.h"
#include "domain/ctr/ncch.h"
#include "domain/ctr/ncsd.h"
#include "domain/ctr/romfs.h"
#include "domain/profile/validation.h"

#include <algorithm>
#include <array>
#include <format>

namespace hexlith::application {
namespace {

namespace ctr = domain::ctr;
namespace profile = domain::profile;

// The layout of an extraction, named once so that the tool, the manifest and
// this file cannot disagree about it.
constexpr std::string_view ExHeaderPath = "exheader.bin";
constexpr std::string_view LogoPath = "logo.bin";
constexpr std::string_view PlainRegionPath = "plain.txt";
constexpr std::string_view ExeFsDirectory = "exefs";
constexpr std::string_view RomFsDirectory = "romfs";

// What the parsers made of the image, gathered before anything is written.
struct ParsedImage {
    core::ByteSpan image;
    core::ByteSpan partition;
    ctr::NcsdHeader ncsd;
    ctr::NcchHeader ncch;
    ctr::ExHeader exheader;
    core::ByteSpan exheader_region;
};

core::Result<ParsedImage> ParseImage(core::ByteSpan image, std::size_t partition_index) {
    ParsedImage parsed;
    parsed.image = image;

    const core::Result<ctr::NcsdHeader> ncsd = ctr::ParseNcsdHeader(image);
    if (!ncsd) {
        return ncsd.Failure().WithContext("the cartridge image");
    }
    parsed.ncsd = ncsd.Value();

    const core::Result<core::ByteSpan> partition =
        ctr::PartitionContents(image, parsed.ncsd, partition_index);
    if (!partition) {
        return partition.Failure();
    }
    parsed.partition = partition.Value();

    const core::Result<ctr::NcchHeader> ncch = ctr::ParseNcchHeader(parsed.partition);
    if (!ncch) {
        return ncch.Failure().WithContext(std::format("partition {}", partition_index));
    }
    parsed.ncch = ncch.Value();

    const core::Result<core::ByteSpan> region =
        ctr::RegionContents(parsed.partition, parsed.ncch.exheader, "extended header");
    if (!region) {
        return region.Failure();
    }
    parsed.exheader_region = region.Value();

    const core::Result<ctr::ExHeader> exheader = ctr::ParseExHeader(parsed.exheader_region);
    if (!exheader) {
        return exheader.Failure();
    }
    parsed.exheader = exheader.Value();

    return parsed;
}

core::Status CheckAgainstProfile(const ParsedImage& parsed, const profile::GameProfile& against) {
    const profile::ImageIdentity identity = profile::IdentityOf(parsed.ncch, parsed.exheader);
    const std::vector<std::string> mismatches = profile::Mismatches(against, identity);
    if (mismatches.empty()) {
        return {};
    }

    std::string report = std::format(
        "the image does not match the profile for {} ({} disagreement(s)), so nothing was "
        "extracted",
        against.name,
        mismatches.size());
    for (const std::string& mismatch : mismatches) {
        report.append("\n  ");
        report.append(mismatch);
    }
    return core::Error{std::move(report)};
}

// Writes one blob and records what was written, so that the manifest is built
// from what went to disk rather than from what was intended.
core::Status Emit(ports::BlobSink& sink,
                  ExtractReport& report,
                  std::string_view path,
                  core::ByteSpan data,
                  ContentSource source,
                  bool decompressed) {
    const core::Status written = sink.Write(path, data);
    if (!written) {
        return written.Failure();
    }

    report.files.push_back(ExtractedFile{.path = std::string{path},
                                         .size = data.size(),
                                         .digest = core::Sha256::Of(data),
                                         .source = source,
                                         .decompressed = decompressed});
    return {};
}

core::Status
EmitNcchRegions(ports::BlobSink& sink, const ParsedImage& parsed, ExtractReport& report) {
    const core::Status exheader =
        Emit(sink, report, ExHeaderPath, parsed.exheader_region, ContentSource::Ncch, false);
    if (!exheader) {
        return exheader.Failure();
    }

    struct OptionalRegion {
        const ctr::NcchRegion* region;
        std::string_view path;
        std::string_view name;
    };

    const std::array<OptionalRegion, 2> regions = {{
        {.region = &parsed.ncch.logo, .path = LogoPath, .name = "logo region"},
        {.region = &parsed.ncch.plain, .path = PlainRegionPath, .name = "plain region"},
    }};

    for (const OptionalRegion& entry : regions) {
        if (!entry.region->Present()) {
            continue;
        }
        const core::Result<core::ByteSpan> contents =
            ctr::RegionContents(parsed.partition, *entry.region, entry.name);
        if (!contents) {
            return contents.Failure();
        }
        const core::Status written =
            Emit(sink, report, entry.path, contents.Value(), ContentSource::Ncch, false);
        if (!written) {
            return written.Failure();
        }
    }

    return {};
}

core::Status EmitExeFs(ports::BlobSink& sink, const ParsedImage& parsed, ExtractReport& report) {
    if (!parsed.ncch.exefs.Present()) {
        return core::Error{"the partition declares no ExeFS, so there is no executable to lift"};
    }

    const core::Result<core::ByteSpan> region =
        ctr::RegionContents(parsed.partition, parsed.ncch.exefs, "ExeFS");
    if (!region) {
        return region.Failure();
    }

    const core::Result<ctr::ExeFs> filesystem = ctr::ParseExeFs(region.Value());
    if (!filesystem) {
        return filesystem.Failure();
    }

    const core::Status directory = sink.MakeDirectory(ExeFsDirectory);
    if (!directory) {
        return directory.Failure();
    }

    for (const ctr::ExeFsFile& file : filesystem.Value().files) {
        const core::Result<core::ByteSpan> contents = ctr::ExeFsFileContents(region.Value(), file);
        if (!contents) {
            return contents.Failure();
        }

        const std::string path = std::format("{}/{}", ExeFsDirectory, file.name);

        // Only .code is ever compressed, and only the extended header says
        // so. Nothing in the ExeFS itself indicates it.
        const bool compressed =
            parsed.exheader.compress_exefs_code && file.name == ctr::exefs::CodeFileName;
        if (!compressed) {
            const core::Status written =
                Emit(sink, report, path, contents.Value(), ContentSource::ExeFs, false);
            if (!written) {
                return written.Failure();
            }
            continue;
        }

        const core::Result<std::vector<std::uint8_t>> decompressed =
            ctr::DecompressBackwardsLzss(contents.Value());
        if (!decompressed) {
            return decompressed.Failure().WithContext("the .code section");
        }

        const std::size_t expected = parsed.exheader.DecompressedCodeSize();
        if (decompressed.Value().size() != expected) {
            return core::Error{std::format(
                "the .code section decompressed to {:#x} bytes and the extended header's "
                "page-aligned segments sum to {:#x}",
                decompressed.Value().size(),
                expected)};
        }

        const core::Status written = Emit(
            sink, report, path, core::ByteSpan{decompressed.Value()}, ContentSource::ExeFs, true);
        if (!written) {
            return written.Failure();
        }
    }

    return {};
}

core::Status EmitRomFs(ports::BlobSink& sink, const ParsedImage& parsed, ExtractReport& report) {
    if (!parsed.ncch.romfs.Present()) {
        return core::Error{"the partition declares no RomFS"};
    }

    const core::Result<core::ByteSpan> region =
        ctr::RegionContents(parsed.partition, parsed.ncch.romfs, "RomFS");
    if (!region) {
        return region.Failure();
    }

    const core::Result<ctr::RomFs> filesystem = ctr::ParseRomFs(region.Value());
    if (!filesystem) {
        return filesystem.Failure();
    }

    report.romfs_file_count = filesystem.Value().files.size();
    report.romfs_directory_count = filesystem.Value().directories.size();

    const core::Status root = sink.MakeDirectory(RomFsDirectory);
    if (!root) {
        return root.Failure();
    }
    for (const std::string& directory : filesystem.Value().directories) {
        const core::Status made =
            sink.MakeDirectory(std::format("{}/{}", RomFsDirectory, directory));
        if (!made) {
            return made.Failure();
        }
    }

    for (const ctr::RomFsFile& file : filesystem.Value().files) {
        const core::Result<core::ByteSpan> contents = ctr::RomFsFileContents(region.Value(), file);
        if (!contents) {
            return contents.Failure();
        }

        const std::string path = std::format("{}/{}", RomFsDirectory, file.path);
        const core::Status written =
            Emit(sink, report, path, contents.Value(), ContentSource::RomFs, false);
        if (!written) {
            return written.Failure();
        }

        // A module is one because of the magic at 0x80, not because of its
        // extension. static.crs is one and .crr/static.crr is not.
        if (!ctr::LooksLikeCroModule(contents.Value())) {
            continue;
        }
        const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(contents.Value());
        if (!header) {
            return header.Failure().WithContext(file.path);
        }
        report.modules.push_back(
            ExtractedModule{.path = std::string{file.path}, .header = header.Value()});
    }

    return {};
}

core::Status CheckWhatWasWritten(const ExtractReport& report, const profile::GameProfile& against) {
    std::vector<std::string> problems;

    if (report.romfs_file_count != against.romfs_file_count) {
        problems.push_back(std::format("the profile expects {} RomFS file(s) and {} were found",
                                       against.romfs_file_count,
                                       report.romfs_file_count));
    }
    if (report.modules.size() != against.cro_module_count) {
        problems.push_back(std::format("the profile expects {} CRO module(s) and {} were found",
                                       against.cro_module_count,
                                       report.modules.size()));
    }

    const auto code = std::ranges::find_if(
        report.files, [](const ExtractedFile& file) { return file.decompressed; });
    if (code == report.files.end()) {
        problems.emplace_back("nothing was decompressed, so there is no .code section");
    } else {
        if (code->size != against.code_size) {
            problems.push_back(
                std::format("the profile expects a {:#x} byte .code section and {:#x} was written",
                            against.code_size,
                            code->size));
        }
        if (code->digest.ToHex() != against.code_digest) {
            problems.push_back(std::format("the .code section hashes to {} and the profile says {}",
                                           code->digest.ToHex(),
                                           against.code_digest));
        }
    }

    if (problems.empty()) {
        return {};
    }

    std::string report_text =
        std::format("what was extracted does not match the profile for {}", against.name);
    for (const std::string& problem : problems) {
        report_text.append("\n  ");
        report_text.append(problem);
    }
    return core::Error{std::move(report_text)};
}

}  // namespace

std::string_view Describe(ContentSource source) {
    switch (source) {
    case ContentSource::Ncch:
        return "ncch";
    case ContentSource::ExeFs:
        return "exefs";
    case ContentSource::RomFs:
        return "romfs";
    }
    return "unknown";
}

std::size_t ExtractReport::TotalWrittenBytes() const {
    std::size_t total = 0;
    for (const ExtractedFile& file : files) {
        total += file.size;
    }
    return total;
}

core::Result<ExtractReport>
Extract(ports::RomSource& source, ports::BlobSink& sink, const ExtractRequest& request) {
    if (request.profile == nullptr) {
        return core::Error{"no profile was given, and an image is never extracted on a guess"};
    }

    const core::Result<core::ByteSpan> image = source.Contents();
    if (!image) {
        return image.Failure().WithContext(source.Describe());
    }

    const core::Result<ParsedImage> parsed =
        ParseImage(image.Value(), request.profile->executable_partition);
    if (!parsed) {
        return parsed.Failure();
    }

    // Validation comes before anything is written, so a refused image leaves
    // the work directory as it was.
    if (request.require_profile_match) {
        const core::Status matches = CheckAgainstProfile(parsed.Value(), *request.profile);
        if (!matches) {
            return matches.Failure();
        }
    }

    ExtractReport report;
    report.profile = request.profile;
    report.image_digest = core::Sha256::Of(image.Value());
    report.image_size = image.Value().size();
    report.partition_index = request.profile->executable_partition;
    report.partition_offset = parsed.Value().ncsd.Partition(report.partition_index).offset;
    report.partition_size = parsed.Value().ncsd.Partition(report.partition_index).size;

    const core::Status regions = EmitNcchRegions(sink, parsed.Value(), report);
    if (!regions) {
        return regions.Failure();
    }
    const core::Status exefs = EmitExeFs(sink, parsed.Value(), report);
    if (!exefs) {
        return exefs.Failure();
    }
    const core::Status romfs = EmitRomFs(sink, parsed.Value(), report);
    if (!romfs) {
        return romfs.Failure();
    }

    // The manifest has to read the same way twice, and the order files come
    // out of the containers is not the order anybody would look for them in.
    std::ranges::sort(report.files, [](const ExtractedFile& left, const ExtractedFile& right) {
        return left.path < right.path;
    });
    std::ranges::sort(report.modules,
                      [](const ExtractedModule& left, const ExtractedModule& right) {
                          return left.path < right.path;
                      });

    if (request.require_profile_match) {
        const core::Status written = CheckWhatWasWritten(report, *request.profile);
        if (!written) {
            return written.Failure();
        }
    }

    return report;
}

}  // namespace hexlith::application
