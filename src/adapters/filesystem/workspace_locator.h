#ifndef HEXLITH_ADAPTERS_FILESYSTEM_WORKSPACE_LOCATOR_H
#define HEXLITH_ADAPTERS_FILESYSTEM_WORKSPACE_LOCATOR_H

#include "application/workspace.h"

#include <optional>
#include <string>

namespace hexlith::adapters::filesystem {

// Gathers what workspace resolution needs from the environment and the disk.
//
// The resolution itself is in src/application and reads neither, which is
// what lets the precedence rules be tested without a workspace. This is the
// half that has to touch the outside world, and it is as thin as it can be:
// read two variables, work out where the configuration file would live, read
// it if it is there.
struct WorkspaceArguments {
    std::optional<std::string> rom_directory;
    std::optional<std::string> work_directory;
};

application::WorkspaceInputs GatherWorkspaceInputs(const WorkspaceArguments& arguments);

core::Result<application::Workspace> LocateWorkspace(const WorkspaceArguments& arguments);

core::Result<application::ResolvedPath> LocateWorkDirectory(const WorkspaceArguments& arguments);

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_WORKSPACE_LOCATOR_H
