#include "application/workspace.h"

#include "core/key_value_text.h"

#include <format>
#include <utility>
#include <vector>

namespace hexlith::application {
namespace {

using core::Error;
using core::KeyValue;

// One place a path could have come from, in precedence order once several are
// put in a list.
struct Candidate {
    std::string description;
    std::optional<std::string> value;
    PathSource source;

    // A path from a configuration file or an environment variable has no
    // directory to be relative to, since the one the tool happens to be
    // started from is not it. A command-line argument does have one, which is
    // the shell the user is standing in, so that one is left alone.
    bool must_be_absolute;
};

core::Status RequireUsable(std::string_view value, std::string_view origin, bool must_be_absolute) {
    if (value.empty()) {
        return Error{std::format("{} is empty", origin)};
    }
    if (!must_be_absolute) {
        return {};
    }
    if (value.front() == '~') {
        return Error{std::format(
            "{} starts with '~', which only a shell expands. Write the path out in full", origin)};
    }
    if (!std::filesystem::path{value}.is_absolute()) {
        return Error{std::format("{} is '{}', which is relative. It has to be absolute, since "
                                 "there is no directory for it to be relative to",
                                 origin,
                                 value)};
    }
    return {};
}

core::Result<ResolvedPath> FirstProvided(std::string_view what,
                                         const std::vector<Candidate>& candidates) {
    std::string consulted;
    for (const Candidate& candidate : candidates) {
        if (candidate.value.has_value()) {
            const core::Status usable =
                RequireUsable(*candidate.value, candidate.description, candidate.must_be_absolute);
            if (!usable) {
                return usable.Failure();
            }
            return ResolvedPath{.path = std::filesystem::path{*candidate.value},
                                .source = candidate.source};
        }
        if (!consulted.empty()) {
            consulted.append(", ");
        }
        consulted.append(candidate.description);
    }

    return Error{std::format("no {} was given. Consulted {}. Hexlith has no default for it, "
                             "because a default would eventually point somewhere nobody meant",
                             what,
                             consulted)};
}

std::optional<std::string> ConfiguredValue(const std::vector<KeyValue>& entries,
                                           std::string_view key) {
    const KeyValue* entry = core::Find(entries, key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->value;
}

}  // namespace

std::string_view Describe(PathSource source) {
    switch (source) {
    case PathSource::CommandLine:
        return "the command line";
    case PathSource::Environment:
        return "the environment";
    case PathSource::ConfigurationFile:
        return "the configuration file";
    }
    return "an unknown source";
}

core::Result<Workspace> ResolveWorkspace(const WorkspaceInputs& inputs) {
    std::vector<KeyValue> configured;
    if (inputs.configuration_text.has_value()) {
        core::Result<std::vector<KeyValue>> parsed =
            core::ParseKeyValueText(*inputs.configuration_text);
        if (!parsed) {
            return parsed.Failure().WithContext(inputs.configuration_path.string());
        }
        configured = std::move(parsed.Value());
    }

    // Named whether or not the file is there, since "it does not exist" is
    // the single most useful thing a failure can say about it.
    const auto describe_configuration = [&inputs](std::string_view key) {
        if (!inputs.configuration_text.has_value()) {
            return std::format("{}, which does not exist", inputs.configuration_path.string());
        }
        return std::format("{} in {}", key, inputs.configuration_path.string());
    };

    core::Result<ResolvedPath> rom_directory =
        FirstProvided("ROM directory",
                      {
                          Candidate{.description = "the --rom-dir argument",
                                    .value = inputs.rom_directory_argument,
                                    .source = PathSource::CommandLine,
                                    .must_be_absolute = false},
                          Candidate{.description = std::string{RomDirectoryVariable},
                                    .value = inputs.rom_directory_variable,
                                    .source = PathSource::Environment,
                                    .must_be_absolute = true},
                          Candidate{.description = describe_configuration(RomDirectoryKey),
                                    .value = ConfiguredValue(configured, RomDirectoryKey),
                                    .source = PathSource::ConfigurationFile,
                                    .must_be_absolute = true},
                      });
    if (!rom_directory) {
        return rom_directory.Failure();
    }

    core::Result<ResolvedPath> work_directory =
        FirstProvided("work directory",
                      {
                          Candidate{.description = "the --work-dir argument",
                                    .value = inputs.work_directory_argument,
                                    .source = PathSource::CommandLine,
                                    .must_be_absolute = false},
                          Candidate{.description = std::string{WorkDirectoryVariable},
                                    .value = inputs.work_directory_variable,
                                    .source = PathSource::Environment,
                                    .must_be_absolute = true},
                          Candidate{.description = describe_configuration(WorkDirectoryKey),
                                    .value = ConfiguredValue(configured, WorkDirectoryKey),
                                    .source = PathSource::ConfigurationFile,
                                    .must_be_absolute = true},
                      });
    if (!work_directory) {
        return work_directory.Failure();
    }

    return Workspace{.rom_directory = rom_directory.Value(),
                     .work_directory = work_directory.Value()};
}

std::optional<std::filesystem::path>
ConfigurationFilePath(std::optional<std::string_view> xdg_config_home,
                      std::optional<std::string_view> home) {
    if (xdg_config_home.has_value() && !xdg_config_home->empty()) {
        return std::filesystem::path{*xdg_config_home} / ConfigurationFileName;
    }
    if (home.has_value() && !home->empty()) {
        return std::filesystem::path{*home} / ".config" / ConfigurationFileName;
    }
    return std::nullopt;
}

}  // namespace hexlith::application
