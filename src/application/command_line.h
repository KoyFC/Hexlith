#ifndef HEXLITH_APPLICATION_COMMAND_LINE_H
#define HEXLITH_APPLICATION_COMMAND_LINE_H

#include "core/result.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hexlith::application {

struct ExtractOptions {
    std::optional<std::string> rom_directory;
    std::optional<std::string> work_directory;

    // Names the image directly, for a one-off run against something that is
    // not the usual file.
    std::optional<std::string> image;

    // Extracts an image that does not match the profile. For development
    // only, which is why it has to be asked for by name.
    bool allow_mismatch = false;

    bool help = false;
};

struct AnalyzeOptions {
    std::optional<std::string> work_directory;

    // The extraction to analyse, instead of the one the profile names. For a
    // second copy of a workspace, or for a directory that was moved.
    std::optional<std::string> extraction;

    // Where the four tables are written. The default is a directory beside the
    // extraction, which keeps a generated table out of the repository until
    // somebody puts it there on purpose.
    std::optional<std::string> output;

    // Leaves out the sweep of read-only data for words that look like the address
    // of a function. docs/classification.md has what that costs.
    bool no_sweep = false;

    bool help = false;
};

// Parses the arguments of hexlith-extract.
//
// This lives in the application layer rather than in the tool so that it has
// tests. A main that wires adapters to a use case has nothing in it worth
// testing; one that also parses arguments has, and then does not get it.
core::Result<ExtractOptions> ParseExtractCommandLine(std::span<const std::string_view> arguments);

std::string ExtractUsage();

core::Result<AnalyzeOptions> ParseAnalyzeCommandLine(std::span<const std::string_view> arguments);

std::string AnalyzeUsage();

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_COMMAND_LINE_H
