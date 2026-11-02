#ifndef HEXLITH_TOOLS_COMMON_TOOL_MAIN_H
#define HEXLITH_TOOLS_COMMON_TOOL_MAIN_H

#include "core/result.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What every tool's main does around its own work.
namespace hexlith::tools {

constexpr int ExitSuccess = 0;
constexpr int ExitFailure = 1;
constexpr int ExitUsage = 2;

// Runs a tool's Main on its command line. Nothing below a Main throws as a
// matter of course, since failures are a Result; what does escape, most likely
// a std::bad_alloc, deserves a sentence naming the tool rather than a
// terminate.
template<class Main>
int RunGuarded(std::string_view tool, int argc, char** argv, Main main) {
    try {
        return main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        std::cerr << tool << ": " << thrown.what() << '\n';
    } catch (...) {
        std::cerr << tool << ": something was thrown that is not an exception\n";
    }
    return ExitFailure;
}

// The main of a tool whose options application/command_line.h parses: the
// usage for --help or for a command line it refuses, and otherwise the run.
template<class Options>
int RunCommand(std::string_view tool,
               int argc,
               char** argv,
               core::Result<Options> (*parse)(std::span<const std::string_view>),
               std::string (*usage)(),
               int (*run)(const Options&)) {
    return RunGuarded(tool, argc, argv, [&](std::span<char* const> command_line) {
        // Everything after the program's name, which a caller may leave out.
        const std::span<char* const> given = command_line.subspan(command_line.empty() ? 0 : 1);
        const std::vector<std::string_view> arguments(given.begin(), given.end());
        const core::Result<Options> options = parse(arguments);
        if (!options) {
            std::cerr << tool << ": " << options.Failure().Message() << '\n' << usage();
            return ExitUsage;
        }
        if (options.Value().help) {
            std::cout << usage();
            return ExitSuccess;
        }
        return run(options.Value());
    });
}

}  // namespace hexlith::tools

#endif  // HEXLITH_TOOLS_COMMON_TOOL_MAIN_H
