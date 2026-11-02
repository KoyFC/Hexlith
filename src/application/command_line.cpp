#include "application/command_line.h"

#include "application/workspace.h"

#include <format>
#include <tuple>

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
constexpr std::string_view StepOption = "--step";
constexpr std::string_view CompareOption = "--compare";
constexpr std::string_view TraceOption = "--trace";
constexpr std::string_view NewGameOption = "--new-game";
constexpr std::string_view GoldenOption = "--golden";
constexpr std::string_view InputOption = "--input";
constexpr std::string_view StopAfterOption = "--stop-after";
constexpr std::string_view SurveyOption = "--survey";
constexpr std::string_view EmitOption = "--emit";
constexpr std::string_view ExecutedOption = "--executed";
constexpr std::string_view RecordGpuOption = "--record-gpu";
constexpr std::string_view WindowOption = "--window";
constexpr std::string_view BackendOption = "--backend";
constexpr std::string_view DeviceOption = "--device";
constexpr std::string_view RecordExecutedOption = "--record-executed";
constexpr std::string_view LiftedOption = "--lifted";

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

core::Result<LiftOptions> ParseLiftCommandLine(std::span<const std::string_view> arguments) {
    LiftOptions options;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == HelpOption || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument == SurveyOption) {
            options.survey = true;
            continue;
        }
        if (argument == EmitOption) {
            options.emit = true;
            continue;
        }

        core::Status taken;
        if (argument == WorkDirectoryOption) {
            taken = TakeValue(arguments, index, options.work_directory);
        } else if (argument == ExtractionOption) {
            taken = TakeValue(arguments, index, options.extraction);
        } else if (argument == ExecutedOption) {
            taken = TakeValue(arguments, index, options.executed);
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

std::string LiftUsage() {
    return std::format(
        "hexlith-lift: turns the analysed code of the main executable into C++ in the\n"
        "workspace.\n"
        "\n"
        "  {} <path>     where the extraction lives\n"
        "  {} <path>   a specific extraction directory, instead of the profile's\n"
        "  {}              cut every function into blocks and print what the control\n"
        "                       flow of the whole executable looks like\n"
        "  {} <path>      weigh it by a table of executed instructions\n"
        "  {}                write the lifted C++ into the workspace\n"
        "  {} <path>        where it goes, instead of the lift directory beside the extraction\n"
        "  {}                this text\n"
        "\n"
        "The work directory is taken from the command line first, then from {},\n"
        "then from the configuration file, the same way hexlith-extract takes it.\n",
        WorkDirectoryOption,
        ExtractionOption,
        SurveyOption,
        ExecutedOption,
        EmitOption,
        OutputOption,
        HelpOption,
        WorkDirectoryVariable);
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
    if (argument == RecordExecutedOption) {
        return &options.record_executed;
    }
    if (argument == WindowOption) {
        return &options.window;
    }
    return nullptr;
}

core::Status
TakeBackend(std::span<const std::string_view> arguments, std::size_t& index, bool& vulkan) {
    std::optional<std::string> name;
    core::Status taken = TakeValue(arguments, index, name);
    if (!taken || !name.has_value()) {
        return taken;
    }
    if (*name != "reference" && *name != "vulkan") {
        return core::Error{
            std::format("{} is either reference or vulkan, not '{}'", BackendOption, *name)};
    }
    vulkan = *name == "vulkan";
    return {};
}

core::Status
TakeDevice(std::span<const std::string_view> arguments, std::size_t& index, std::string& device) {
    std::optional<std::string> name;
    core::Status taken = TakeValue(arguments, index, name);
    if (taken && name.has_value()) {
        device = *name;
    }
    return taken;
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
        } else if (argument == LiftedOption) {
            taken = TakeValue(arguments, index, options.lifted);
        } else if (argument == StopAfterOption) {
            taken = TakeStopPoint(arguments, index, options.stop_after);
        } else if (argument == BackendOption) {
            taken = TakeBackend(arguments, index, options.vulkan);
        } else if (argument == DeviceOption) {
            taken = TakeDevice(arguments, index, options.device);
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

    if (options.window && options.record_gpu) {
        return core::Error{std::format("{} and {} both take the GPU's events, so only one of them "
                                       "can be given",
                                       WindowOption,
                                       RecordGpuOption)};
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
        "  {}               show the screens in a window as the title draws them\n"
        "  {} <name>       what draws for the window: reference (the default) or vulkan\n"
        "  {} <name>        the Vulkan device to use, by a part of its name\n"
        "  {}   count the instructions executed, by address, under <work-dir>/<title>/observed\n"
        "  {} <path>      run through the lifted library at that path where it covers the code\n"
        "  {}               this text\n"
        "\n"
        "Without {} the save data under <work-dir>/<title>/save is read at the start and\n"
        "written each time the title commits it, and when the run stops.\n"
        "\n"
        "With {} and no {}, the player is at the keyboard: the arrows are the D-pad, X and Z\n"
        "are A and B, S and A are X and Y, Q and W are L and R, Enter is Start, Backspace is\n"
        "Select, I, J, K and L push the circle pad, and the left mouse button touches the\n"
        "bottom screen.\n"
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
        WindowOption,
        BackendOption,
        DeviceOption,
        RecordExecutedOption,
        LiftedOption,
        HelpOption,
        NewGameOption,
        WindowOption,
        InputOption,
        RomDirectoryVariable,
        WorkDirectoryVariable);
}

core::Result<LiftVerifyOptions>
ParseLiftVerifyCommandLine(std::span<const std::string_view> arguments) {
    LiftVerifyOptions options;
    std::optional<std::string> limit;
    std::optional<std::string> step;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == HelpOption || argument == "-h") {
            options.help = true;
            continue;
        }

        core::Status taken;
        if (argument == RomDirectoryOption) {
            taken = TakeValue(arguments, index, options.rom_directory);
        } else if (argument == WorkDirectoryOption) {
            taken = TakeValue(arguments, index, options.work_directory);
        } else if (argument == ImageOption) {
            taken = TakeValue(arguments, index, options.image);
        } else if (argument == LiftedOption) {
            taken = TakeValue(arguments, index, options.lifted);
        } else if (argument == InputOption) {
            taken = TakeValue(arguments, index, options.input);
        } else if (argument == LimitOption) {
            taken = TakeValue(arguments, index, limit);
        } else if (argument == StepOption) {
            taken = TakeValue(arguments, index, step);
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

    if (options.help) {
        return options;
    }
    if (!options.lifted.has_value()) {
        return core::Error{std::format("{} is required: the library to verify", LiftedOption)};
    }
    for (const auto& [option, text, value] :
         {std::tuple{LimitOption, &limit, &options.instruction_limit},
          std::tuple{StepOption, &step, &options.step}}) {
        if (!text->has_value()) {
            continue;
        }
        const core::Result<std::uint64_t> count = ParseCount(option, **text);
        if (!count) {
            return count.Failure();
        }
        if (count.Value() == 0) {
            return core::Error{std::format("{} needs a number above zero", option)};
        }
        *value = count.Value();
    }
    return options;
}

std::string LiftVerifyUsage() {
    return std::format(
        "hexlith-lift-verify: runs the title through the interpreter and through the lifted\n"
        "library side by side, from a new game, compares the two after every step of\n"
        "instructions, and narrows the first difference down to the instruction.\n"
        "\n"
        "  {} <path>      where the cartridge image is\n"
        "  {} <path>     the work directory, which the workspace requires\n"
        "  {} <path>        a specific image, instead of the one in the ROM directory\n"
        "  {} <path>       the lifted library to verify\n"
        "  {} <path>        a script of the player's input, by frame, for both runs\n"
        "  {} <count>       instructions to verify, {} by default\n"
        "  {} <count>        instructions between comparisons, {} by default\n"
        "  {}               this text\n",
        RomDirectoryOption,
        WorkDirectoryOption,
        ImageOption,
        LiftedOption,
        InputOption,
        LimitOption,
        DefaultVerifyInstructionLimit,
        StepOption,
        DefaultVerifyStep,
        HelpOption);
}

}  // namespace hexlith::application
