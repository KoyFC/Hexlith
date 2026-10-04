#ifndef HEXLITH_APPLICATION_WORKSPACE_H
#define HEXLITH_APPLICATION_WORKSPACE_H

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hexlith::application {

// The names a tool, its help text and a configuration file all have to agree
// on. Kept here so that they cannot drift apart.
constexpr std::string_view RomDirectoryVariable = "HEXLITH_ROM_DIR";
constexpr std::string_view WorkDirectoryVariable = "HEXLITH_WORK_DIR";
constexpr std::string_view RomDirectoryKey = "rom_dir";
constexpr std::string_view WorkDirectoryKey = "work_dir";
constexpr std::string_view ConfigurationFileName = "hexlith/workspace.conf";

// Where a path came from. A tool reports this, because the failure worth
// avoiding is one where the tool used a path the user had forgotten setting.
enum class PathSource : std::uint8_t { CommandLine, Environment, ConfigurationFile };

std::string_view Describe(PathSource source);

struct ResolvedPath {
    std::filesystem::path path;
    PathSource source = PathSource::CommandLine;
};

// Where the cartridge image is read from, and where everything derived from
// it is written. Neither ever points inside the repository, which is why
// there is no default for either. See ADR 0008.
struct Workspace {
    ResolvedPath rom_directory;
    ResolvedPath work_directory;
};

// Everything resolution needs, gathered by the caller. The resolver reads
// neither the environment nor the disk, which is what lets the precedence
// rules be tested on a machine that has no workspace at all.
struct WorkspaceInputs {
    std::optional<std::string> rom_directory_argument;
    std::optional<std::string> work_directory_argument;
    std::optional<std::string> rom_directory_variable;
    std::optional<std::string> work_directory_variable;

    // The contents of workspace.conf, or nothing when there is no such file.
    std::optional<std::string> configuration_text;

    // Where that file was looked for, named in messages whether or not it
    // was found.
    std::filesystem::path configuration_path;
};

// Applies the precedence of ADR 0008: a command-line argument, then an
// environment variable, then the configuration file.
//
// When nothing provides a path the failure names every source that was
// consulted. The mistake being guarded against is a tool that invents a
// location and writes extracted content next to whatever it was run from.
core::Result<Workspace> ResolveWorkspace(const WorkspaceInputs& inputs);

// The work directory alone, for a tool that reads what an extraction produced and
// never touches the cartridge. Demanding a ROM directory it will not read would
// make a tool fail over a path that has nothing to do with it.
core::Result<ResolvedPath> ResolveWorkDirectory(const WorkspaceInputs& inputs);

// Where workspace.conf lives, given the two environment variables that
// decide it. XDG_CONFIG_HOME wins, and $HOME/.config is the fallback the
// specification names. Nothing comes back when neither is set, which means
// there is no configuration file to look for rather than that one is
// missing.
std::optional<std::filesystem::path>
ConfigurationFilePath(std::optional<std::string_view> xdg_config_home,
                      std::optional<std::string_view> home);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_WORKSPACE_H
