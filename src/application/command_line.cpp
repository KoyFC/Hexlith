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
constexpr std::string_view LimitOption = "--limit";
constexpr std::string_view CompareOption = "--compare";
constexpr std::string_view TraceOption = "--trace";
constexpr std::string_view NewGameOption = "--new-game";
constexpr std::string_view GoldenOption = "--golden";
constexpr std::string_view InputOption = "--input";
constexpr std::string_view StopAfterOption = "--stop-after";
constexpr std::string_view RecordGpuOption = "--record-gpu";

constexpr std::string_view LoadEvent = "cro-load:";
constexpr std::string_view UnloadEvent = "cro-unload:";
constexpr std::string_view FrameEvent = "frame:";

// A count, of instructions or frames, in decimal, with no sign and no suffix.
core::Result<std::uint64_t> ParseCount(std::string_view option, std::string_view text) {
    constexpr std::uint64_t Decimal = 10;
    if (text.empty()) {
        return core::Error{std::format("{} needs a number", option)};
    }
    std::uint64_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return core::Error{std::format("{} needs a number and was given {}", option, text)};
        }
        const auto next = static_cast<std::uint64_t>(digit - '0');
        if (value > (UINT64_MAX - next) / Decimal) {
            return core::Error{std::format("{} is too large", text)};
        }
        value = (value * Decimal) + next;
    }
    return value;
}

core::Result<BootStopPoint> ParseStopPoint(std::string_view text) {
    using Kind = BootStopPoint::Kind;
    for (const auto& [prefix, kind] :
         {std::pair{LoadEvent, Kind::ModuleLoad}, std::pair{UnloadEvent, Kind::ModuleUnload}}) {
        if (text.starts_with(prefix) && text.size() > prefix.size()) {
            return BootStopPoint{
                .kind = kind, .module = std::string{text.substr(prefix.size())}, .frame = 0};
        }
    }
    if (text.starts_with(FrameEvent)) {
        const core::Result<std::uint64_t> frame =
            ParseCount(StopAfterOption, text.substr(FrameEvent.size()));
        if (!frame) {
            return frame.Failure();
        }
        return BootStopPoint{.kind = Kind::Frame, .module = {}, .frame = frame.Value()};
    }
    return core::Error{
        std::format("{} takes cro-load:<module>, cro-unload:<module> or frame:<number>, not '{}'",
                    StopAfterOption,
                    text)};
}

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

core::Status TakeStopPoint(std::span<const std::string_view> arguments,
                           std::size_t& index,
                           std::optional<BootStopPoint>& into) {
    std::optional<std::string> text;
    core::Status taken = TakeValue(arguments, index, text);
    if (!taken || !text.has_value()) {
        return taken;
    }
    if (into.has_value()) {
        return core::Error{std::format("{} was given twice", StopAfterOption)};
    }
    core::Result<BootStopPoint> parsed = ParseStopPoint(*text);
    if (!parsed) {
        return parsed.Failure();
    }
    into = std::move(parsed.Value());
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

namespace {

// The options of hexlith-boot that take no value, or nothing for another.
bool* BootFlag(BootOptions& options, std::string_view argument) {
    if (argument == HelpOption || argument == "-h") {
        return &options.help;
    }
    if (argument == TraceOption) {
        return &options.trace;
    }
    if (argument == NewGameOption) {
        return &options.new_game;
    }
    if (argument == RecordGpuOption) {
        return &options.record_gpu;
    }
    return nullptr;
}

}  // namespace

core::Result<BootOptions> ParseBootCommandLine(std::span<const std::string_view> arguments) {
    BootOptions options;
    std::optional<std::string> limit;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (bool* flag = BootFlag(options, argument)) {
            *flag = true;
            continue;
        }

        core::Status taken;
        if (argument == RomDirectoryOption) {
            taken = TakeValue(arguments, index, options.rom_directory);
        } else if (argument == WorkDirectoryOption) {
            taken = TakeValue(arguments, index, options.work_directory);
        } else if (argument == ImageOption) {
            taken = TakeValue(arguments, index, options.image);
        } else if (argument == LimitOption) {
            taken = TakeValue(arguments, index, limit);
        } else if (argument == CompareOption) {
            taken = TakeValue(arguments, index, options.compare);
        } else if (argument == GoldenOption) {
            taken = TakeValue(arguments, index, options.golden);
        } else if (argument == InputOption) {
            taken = TakeValue(arguments, index, options.input);
        } else if (argument == StopAfterOption) {
            taken = TakeStopPoint(arguments, index, options.stop_after);
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

    if (limit.has_value()) {
        const core::Result<std::uint64_t> count = ParseCount(LimitOption, *limit);
        if (!count) {
            return count.Failure();
        }
        options.instruction_limit = count.Value();
    }
    return options;
}

std::string BootUsage() {
    return std::format(
        "hexlith-boot: runs the title from its entry point under the HLE kernel and services,\n"
        "until it stops, and says why it stopped.\n"
        "\n"
        "  {} <path>      where the cartridge image is\n"
        "  {} <path>     the work directory, which the workspace requires\n"
        "  {} <path>        a specific image, instead of the one in the ROM directory\n"
        "  {} <count>       instructions to run at most, {} by default\n"
        "  {} <path>      a directory of memory captured at the moment the run stops,\n"
        "                       one file per range named by its address, to compare with\n"
        "  {}              every service request and system call as it happens\n"
        "  {}           no save data at the start and none written, the same run every time\n"
        "  {} <path>       write the golden trace there, to compare with Azahar's\n"
        "  {} <path>        a script of the player's input, by frame\n"
        "  {} <event>  stop once ldr:ro has done cro-load:<module> or cro-unload:<module>,\n"
        "                       or once frame:<number> has begun\n"
        "  {}         record the GPU's work under <work-dir>/<title>/gpu\n"
        "  {}               this text\n"
        "\n"
        "Without {} the save data under <work-dir>/<title>/save is read at the start and\n"
        "written each time the title commits it, and when the run stops.\n"
        "\n"
        "Paths are taken from the command line first, then from {} and {},\n"
        "then from the configuration file, the same way hexlith-extract takes them.\n",
        RomDirectoryOption,
        WorkDirectoryOption,
        ImageOption,
        LimitOption,
        DefaultBootInstructionLimit,
        CompareOption,
        TraceOption,
        NewGameOption,
        GoldenOption,
        InputOption,
        StopAfterOption,
        RecordGpuOption,
        HelpOption,
        NewGameOption,
        RomDirectoryVariable,
        WorkDirectoryVariable);
}

}  // namespace hexlith::application
