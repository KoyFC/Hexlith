#include "application/command_line.h"

#include "application/workspace.h"

#include <format>

namespace hexlith::application {
namespace {

constexpr std::string_view RomDirectoryOption = "--rom-dir";
constexpr std::string_view WorkDirectoryOption = "--work-dir";
constexpr std::string_view ImageOption = "--image";
constexpr std::string_view AllowMismatchOption = "--allow-mismatch";
constexpr std::string_view HelpOption = "--help";
constexpr std::string_view ExtractionOption = "--extraction";
constexpr std::string_view OutputOption = "--out";
constexpr std::string_view NoSweepOption = "--no-sweep";

core::Status TakeValue(std::span<const std::string_view> arguments,
                       std::size_t& index,
                       std::optional<std::string>& into) {
    const std::string_view option = arguments[index];
    if (into.has_value()) {
        return core::Error{std::format("{} was given twice", option)};
    }
    if (index + 1 >= arguments.size()) {
        return core::Error{std::format("{} needs a value", option)};
    }

    const std::string_view value = arguments[index + 1];
    if (value.starts_with("--")) {
        return core::Error{std::format("{} needs a value and was followed by {}", option, value)};
    }

    into = std::string{value};
    ++index;
    return {};
}

}  // namespace

core::Result<ExtractOptions> ParseExtractCommandLine(std::span<const std::string_view> arguments) {
    ExtractOptions options;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == HelpOption || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == AllowMismatchOption) {
            options.allow_mismatch = true;
            continue;
        }

        core::Status taken;
        if (argument == RomDirectoryOption) {
            taken = TakeValue(arguments, index, options.rom_directory);
        } else if (argument == WorkDirectoryOption) {
            taken = TakeValue(arguments, index, options.work_directory);
        } else if (argument == ImageOption) {
            taken = TakeValue(arguments, index, options.image);
        } else if (argument.starts_with("-")) {
            return core::Error{std::format("{} is not an option this tool knows", argument)};
        } else {
            return core::Error{std::format(
                "'{}' is not an option. Every path is given with the option it belongs to",
                argument)};
        }

        if (!taken) {
            return taken.Failure();
        }
    }

    return options;
}

core::Result<AnalyzeOptions> ParseAnalyzeCommandLine(std::span<const std::string_view> arguments) {
    AnalyzeOptions options;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == HelpOption || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == NoSweepOption) {
            options.no_sweep = true;
            continue;
        }

        core::Status taken;
        if (argument == WorkDirectoryOption) {
            taken = TakeValue(arguments, index, options.work_directory);
        } else if (argument == ExtractionOption) {
            taken = TakeValue(arguments, index, options.extraction);
        } else if (argument == OutputOption) {
            taken = TakeValue(arguments, index, options.output);
        } else if (argument.starts_with("-")) {
            return core::Error{std::format("{} is not an option this tool knows", argument)};
        } else {
            return core::Error{std::format(
                "'{}' is not an option. Every path is given with the option it belongs to",
                argument)};
        }

        if (!taken) {
            return taken.Failure();
        }
    }

    return options;
}

std::string AnalyzeUsage() {
    return std::format(
        "hexlith-analyze: reads an extraction and writes the analysis tables of ADR 0006.\n"
        "\n"
        "  {} <path>     where the extraction lives\n"
        "  {} <path>   a specific extraction directory, instead of the profile's\n"
        "  {} <path>          where the tables are written\n"
        "  {}            leave out the sweep of data for entry points\n"
        "  {}                this text\n"
        "\n"
        "The work directory is taken from the command line first, then from {},\n"
        "then from the configuration file, the same way hexlith-extract takes it.\n"
        "\n"
        "The tables are written beside the extraction and not into this repository. A\n"
        "table belongs in the repository once it holds something nothing can derive\n"
        "again, which is a decision for a person and not for a tool.\n",
        WorkDirectoryOption,
        ExtractionOption,
        OutputOption,
        NoSweepOption,
        HelpOption,
        WorkDirectoryVariable);
}

std::string ExtractUsage() {
    return std::format(
        "hexlith-extract: validates a cartridge image against a game profile and writes its\n"
        "contents into the workspace.\n"
        "\n"
        "  {} <path>      where the cartridge image lives\n"
        "  {} <path>     where everything derived from it is written\n"
        "  {} <path>        a specific image, instead of the one in the ROM directory\n"
        "  {}       extract an image that does not match the profile\n"
        "  {}                this text\n"
        "\n"
        "Paths are taken from the command line first, then from {} and\n"
        "{}, then from the configuration file. There is no default for\n"
        "either, because a default would eventually point somewhere nobody meant.\n"
        "\n"
        "The image is opened read-only and is never modified, moved or copied into the\n"
        "repository.\n",
        RomDirectoryOption,
        WorkDirectoryOption,
        ImageOption,
        AllowMismatchOption,
        HelpOption,
        RomDirectoryVariable,
        WorkDirectoryVariable);
}

}  // namespace hexlith::application
