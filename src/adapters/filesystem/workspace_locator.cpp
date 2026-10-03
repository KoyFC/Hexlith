#include "adapters/filesystem/workspace_locator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

// An unset variable and one set to nothing are the same thing here. A tool
// run from a script that exported an empty value should not be told the
// value it gave is wrong.
std::optional<std::string> Variable(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
}

std::optional<std::string> ReadWholeFile(const fs::path& path) {
    std::error_code failure;
    if (!fs::is_regular_file(path, failure)) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

application::WorkspaceInputs GatherWorkspaceInputs(const WorkspaceArguments& arguments) {
    application::WorkspaceInputs inputs;
    inputs.rom_directory_argument = arguments.rom_directory;
    inputs.work_directory_argument = arguments.work_directory;
    inputs.rom_directory_variable =
        Variable(std::string{application::RomDirectoryVariable}.c_str());
    inputs.work_directory_variable =
        Variable(std::string{application::WorkDirectoryVariable}.c_str());

    const std::optional<fs::path> configuration =
        application::ConfigurationFilePath(Variable("XDG_CONFIG_HOME"), Variable("HOME"));
    if (configuration.has_value()) {
        inputs.configuration_path = *configuration;
        inputs.configuration_text = ReadWholeFile(*configuration);
    } else {
        // Named anyway, so that a failure can say the file was looked for and
        // where it would have been if the environment said.
        inputs.configuration_path = "$XDG_CONFIG_HOME/hexlith/workspace.conf";
    }

    return inputs;
}

core::Result<application::Workspace> LocateWorkspace(const WorkspaceArguments& arguments) {
    return application::ResolveWorkspace(GatherWorkspaceInputs(arguments));
}

}  // namespace hexlith::adapters::filesystem
