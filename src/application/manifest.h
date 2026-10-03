#ifndef HEXLITH_APPLICATION_MANIFEST_H
#define HEXLITH_APPLICATION_MANIFEST_H

#include "application/extract.h"

#include <string>
#include <string_view>

namespace hexlith::application {

// The two files an extraction leaves beside what it wrote.
constexpr std::string_view ManifestPath = "manifest.txt";
constexpr std::string_view ModulesPath = "modules.txt";

// The version of the format, so that a later tool reading an older
// extraction can say so instead of misreading it.
constexpr int ManifestFormatVersion = 1;

// Renders the manifest: one record per line, the kind first, then key=value
// fields, with path= last so that everything after it is the path verbatim.
//
// Two runs over the same image have to produce the same bytes. That rules out
// anything from the clock, anything from the order the host filesystem
// returns entries in, and absolute paths. Records are sorted by path and
// every number is fixed-width hexadecimal.
//
// The format follows ADR 0006: line oriented, diffable, greppable, and
// parseable with code the project owns.
std::string RenderManifest(const ExtractReport& report);

// Renders the summary of every CRO-family module found in the RomFS.
//
// The modules are not copied into a directory of their own. They already live
// in the RomFS and extracting it extracts them, so a second copy would be a
// second thing to keep in step. This file is the index that says which of the
// extracted files are modules and what their headers declare.
std::string RenderModules(const ExtractReport& report);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_MANIFEST_H
