#include "application/manifest.h"

#include "domain/profile/game_profile.h"

#include <format>

namespace hexlith::application {
namespace {

namespace ctr = domain::ctr;

std::string YesOrNo(bool value) {
    return value ? "yes" : "no";
}

}  // namespace

std::string RenderManifest(const ExtractReport& report) {
    std::string text;

    text.append("# Written by hexlith-extract. Every number is hexadecimal.\n");
    text.append("# One record per line: the kind, then key=value fields, with path= last so\n");
    text.append("# that the rest of the line is the path.\n");
    text.append(std::format("manifest-version value={}\n", ManifestFormatVersion));

    if (report.profile != nullptr) {
        text.append(std::format("profile program-id={:016X} remaster-version={:02} name={}\n",
                                report.profile->program_id,
                                report.profile->remaster_version,
                                report.profile->name));
        text.append(std::format("profile-content product-code={} title={}\n",
                                report.profile->product_code,
                                report.profile->application_title));
    }

    text.append(std::format(
        "image size={:#x} digest={}\n", report.image_size, report.image_digest.ToHex()));
    text.append(std::format("partition index={} offset={:#x} size={:#x}\n",
                            report.partition_index,
                            report.partition_offset,
                            report.partition_size));
    text.append(std::format(
        "romfs files={} directories={}\n", report.romfs_file_count, report.romfs_directory_count));
    text.append(std::format("modules count={}\n", report.modules.size()));
    text.append(std::format(
        "totals files={} bytes={:#x}\n", report.files.size(), report.TotalWrittenBytes()));

    for (const ExtractedFile& file : report.files) {
        text.append(std::format("file size={:#x} digest={} source={} decompressed={} path={}\n",
                                file.size,
                                file.digest.ToHex(),
                                Describe(file.source),
                                YesOrNo(file.decompressed),
                                file.path));
    }

    return text;
}

std::string RenderModules(const ExtractReport& report) {
    std::string text;

    text.append("# Written by hexlith-extract. Every number is hexadecimal except the\n");
    text.append("# counts, which are decimal because they are counts.\n");
    text.append("#\n");
    text.append("# The modules themselves are not copied here. They live in the RomFS and\n");
    text.append("# the manifest records them as files like any other; this is the index of\n");
    text.append("# which of those files are modules and what their headers declare.\n");
    text.append(std::format("manifest-version value={}\n", ManifestFormatVersion));
    text.append(std::format("modules count={}\n", report.modules.size()));

    for (const ExtractedModule& module : report.modules) {
        const ctr::CroHeader& header = module.header;

        text.append(
            std::format("module name={} static={} fixed-up={} segments={} code={:#x} rodata={:#x} "
                        "data={:#x} bss={:#x} path={}\n",
                        header.name,
                        YesOrNo(header.IsStaticModule()),
                        YesOrNo(header.fixed_up),
                        header.segments.size(),
                        header.SegmentBytes(ctr::CroSegmentType::Code),
                        header.SegmentBytes(ctr::CroSegmentType::ReadOnlyData),
                        header.SegmentBytes(ctr::CroSegmentType::Data),
                        header.SegmentBytes(ctr::CroSegmentType::Bss),
                        module.path));

        text.append(std::format(
            "module-tables named-exports={} indexed-exports={} export-tree-nodes={} "
            "import-modules={} named-imports={} indexed-imports={} anonymous-imports={} "
            "static-anonymous-symbols={} internal-relocations={} external-relocations={} "
            "static-relocations={} path={}\n",
            header.counts.named_exports,
            header.counts.indexed_exports,
            header.counts.export_tree_nodes,
            header.counts.import_modules,
            header.counts.named_imports,
            header.counts.indexed_imports,
            header.counts.anonymous_imports,
            header.counts.static_anonymous_symbols,
            header.counts.internal_relocations,
            header.counts.external_relocations,
            header.counts.static_relocations,
            module.path));

        for (const ctr::CroSegment& segment : header.segments) {
            text.append(std::format(
                "module-segment index={} type={} raw-type={} offset={:#x} size={:#x} path={}\n",
                segment.index,
                Describe(segment.type),
                segment.raw_type,
                segment.offset,
                segment.size,
                module.path));
        }
    }

    return text;
}

}  // namespace hexlith::application
