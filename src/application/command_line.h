#ifndef HEXLITH_APPLICATION_COMMAND_LINE_H
#define HEXLITH_APPLICATION_COMMAND_LINE_H

#include "core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::application {

// Two billion instructions, about eight seconds of the console's time.
constexpr std::uint64_t DefaultBootInstructionLimit = 2'000'000'000;

// What hexlith-lift-verify runs by default: a little more than the boot to the
// title screen with the usual input script, which is 41,972,790 instructions.
constexpr std::uint64_t DefaultVerifyInstructionLimit = 42'000'000;

// How many instructions the two runs go between comparisons. Each comparison
// reads every mapped page of both, so a step much shorter makes the run slow.
constexpr std::uint64_t DefaultVerifyStep = 100'000;

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

struct LiftOptions {
    std::optional<std::string> work_directory;

    // The extraction to read, instead of the one the profile names.
    std::optional<std::string> extraction;

    // Cuts every function in the analysis tables into blocks and prints what the
    // control flow of the whole executable looks like.
    bool survey = false;

    // Writes the lifted C++ of the main executable into the workspace.
    bool emit = false;

    // Writes the lifted C++ of the CRO modules into the workspace, in a tree of
    // its own, so that a change to one library does not rebuild the other.
    bool emit_modules = false;

    // Where, instead of the lift directory beside the extraction (or its modules
    // directory, for the modules).
    std::optional<std::string> output;

    // The table of executed instructions to weigh the survey by, instead of the
    // one hexlith-boot --record-executed left beside the extraction.
    std::optional<std::string> executed;

    bool help = false;
};

// What to stop the run after, as --stop-after names it: a module event, or
// the vertical blank that starts a frame.
struct BootStopPoint {
    enum class Kind : std::uint8_t { ModuleLoad, ModuleUnload, Frame };
    Kind kind = Kind::ModuleLoad;
    std::string module;
    std::uint64_t frame = 0;
};

struct BootOptions {
    std::optional<std::string> rom_directory;
    std::optional<std::string> work_directory;
    std::optional<std::string> image;

    // How many instructions to run at most. The default is enough to reach the
    // first module and well past it.
    std::uint64_t instruction_limit = DefaultBootInstructionLimit;

    // A directory of memory captured elsewhere, one file per range named by
    // its address in hexadecimal, to compare the process's memory with once
    // the run stops.
    std::optional<std::string> compare;

    // Prints every request to a service and every system call as it happens.
    bool trace = false;

    // Starts from no save data and writes none, so that the run is the same
    // every time. Otherwise the save data under the work directory is read
    // at the start and written on every commit and at the end.
    bool new_game = false;

    // Where to write the golden trace of docs/golden-traces.md, if anywhere.
    std::optional<std::string> golden;

    // A script of the player's input, in the format of
    // docs/formats/input-script.md. Without one nothing is ever pressed.
    std::optional<std::string> input;

    // Stops the run once ldr:ro has loaded or unloaded a module, where a
    // breakpoint after the game's request would stop Azahar, or once a frame
    // has begun.
    std::optional<BootStopPoint> stop_after;

    // Records the GPU's work under the work directory, as
    // docs/formats/gpu-recording.md describes.
    bool record_gpu = false;

    // Shows the screens in a window as the title draws them. It takes the GPU's
    // events in the place of --record-gpu, which it cannot be combined with.
    bool window = false;

    // What draws for the window: the reference renderer, or the Vulkan backend
    // on the device whose name has this in it, or the first one when it is empty.
    bool vulkan = false;
    std::string device;

    // Draws with the one fragment shader that reads the stage's configuration
    // from the registers, instead of one made for each configuration, so that the
    // two can be timed against each other (ADR 0031).
    bool generic_fragments = false;

    // Frames at which the run prints the wall time and the game's own time so
    // far, so that a script can tell what each stretch of a boot cost. Kept in
    // ascending order.
    std::vector<std::uint64_t> time_frames;

    // Frames whose two screens the window's run also writes as PNG files in a
    // directory, so that a live run's pictures can be compared with another's.
    // Both are given or neither, and they need the window. Kept in ascending
    // order.
    std::vector<std::uint64_t> save_frames;
    std::optional<std::string> save_frames_directory;

    // Counts every instruction the run executes by address and writes the table
    // under <work-dir>/<title>/observed, as docs/analysis-tables.md describes.
    // Slows the run by a virtual call per instruction.
    bool record_executed = false;

    // Runs the title through the lifted library at this path where it covers the
    // code, and through the interpreter elsewhere.
    std::optional<std::string> lifted;

    bool help = false;
};

struct LiftVerifyOptions {
    std::optional<std::string> rom_directory;
    std::optional<std::string> work_directory;
    std::optional<std::string> image;

    // The lifted library to verify, which is required.
    std::optional<std::string> lifted;

    // A script of the player's input, the same for both runs.
    std::optional<std::string> input;

    std::uint64_t instruction_limit = DefaultVerifyInstructionLimit;
    std::uint64_t step = DefaultVerifyStep;

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

core::Result<LiftOptions> ParseLiftCommandLine(std::span<const std::string_view> arguments);

std::string LiftUsage();

core::Result<BootOptions> ParseBootCommandLine(std::span<const std::string_view> arguments);

std::string BootUsage();

core::Result<LiftVerifyOptions>
ParseLiftVerifyCommandLine(std::span<const std::string_view> arguments);

std::string LiftVerifyUsage();

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_COMMAND_LINE_H
