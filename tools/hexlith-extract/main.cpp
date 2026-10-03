#include "adapters/filesystem/cartridge_image.h"
#include "adapters/filesystem/directory_blob_sink.h"
#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/workspace_locator.h"
#include "application/command_line.h"
#include "application/extract.h"
#include "application/manifest.h"
#include "application/workspace.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-extract wires the adapters to the extract use case and does
// nothing else. Argument parsing lives in the application layer so that it
// has tests, and every decision worth making is made below this file.
namespace {

namespace adapters = hexlith::adapters::filesystem;
namespace application = hexlith::application;
namespace core = hexlith::core;
namespace profile = hexlith::domain::profile;

constexpr int Success = 0;
constexpr int Failure = 1;
constexpr int Usage = 2;

void Report(std::string_view message) {
    std::cerr << "hexlith-extract: " << message << '\n';
}

// The only title the build knows. A second one would make this a choice, and
// ADR 0005 keys the registry so that the choice can be made explicitly
// rather than by taking the first entry.
const profile::GameProfile* OnlyProfile() {
    if (profile::KnownProfiles().size() != 1) {
        return nullptr;
    }
    return profile::KnownProfiles().front();
}

core::Result<std::filesystem::path> ChooseImage(const application::ExtractOptions& options,
                                                const application::Workspace& workspace) {
    if (options.image.has_value()) {
        std::cout << std::format("image      {} (named on the command line)\n", *options.image);
        return std::filesystem::path{*options.image};
    }

    const core::Result<std::filesystem::path> found =
        adapters::FindCartridgeImage(workspace.rom_directory.path);
    if (!found) {
        return found.Failure();
    }

    std::cout << std::format("image      {} (ROM directory from {})\n",
                             found.Value().string(),
                             application::Describe(workspace.rom_directory.source));
    return found.Value();
}

int Run(const application::ExtractOptions& options) {
    const profile::GameProfile* target = OnlyProfile();
    if (target == nullptr) {
        Report("this build knows more than one profile and cannot choose between them");
        return Failure;
    }

    const core::Result<application::Workspace> workspace =
        adapters::LocateWorkspace(adapters::WorkspaceArguments{
            .rom_directory = options.rom_directory, .work_directory = options.work_directory});
    if (!workspace) {
        Report(workspace.Failure().Message());
        return Failure;
    }

    const core::Result<std::filesystem::path> image = ChooseImage(options, workspace.Value());
    if (!image) {
        Report(image.Failure().Message());
        return Failure;
    }

    const std::filesystem::path destination =
        workspace.Value().work_directory.path / profile::WorkspaceDirectoryName(*target);
    std::cout << std::format("profile    {}\n", target->name);
    std::cout << std::format("writing to {}\n", destination.string());

    adapters::DirectoryBlobSink sink{destination};
    const core::Status prepared = sink.Prepare();
    if (!prepared) {
        Report(prepared.Failure().Message());
        return Failure;
    }

    adapters::FileRomSource source{image.Value()};
    const core::Result<application::ExtractReport> report = application::Extract(
        source,
        sink,
        application::ExtractRequest{.profile = target,
                                    .require_profile_match = !options.allow_mismatch});
    if (!report) {
        Report(report.Failure().Message());
        return Failure;
    }

    const core::Status manifest =
        sink.WriteText(application::ManifestPath, application::RenderManifest(report.Value()));
    if (!manifest) {
        Report(manifest.Failure().Message());
        return Failure;
    }
    const core::Status modules =
        sink.WriteText(application::ModulesPath, application::RenderModules(report.Value()));
    if (!modules) {
        Report(modules.Failure().Message());
        return Failure;
    }

    std::cout << std::format("\n{} file(s), {} byte(s)\n",
                             report.Value().files.size(),
                             report.Value().TotalWrittenBytes());
    std::cout << std::format("{} RomFS file(s) in {} directory(ies)\n",
                             report.Value().romfs_file_count,
                             report.Value().romfs_directory_count);
    std::cout << std::format("{} CRO-family module(s)\n", report.Value().modules.size());
    std::cout << std::format(
        "{} and {} written\n", application::ManifestPath, application::ModulesPath);

    if (options.allow_mismatch) {
        Report("--allow-mismatch was given, so the image was not held to the profile");
    }

    return Success;
}

int Main(std::span<char* const> command_line) {
    std::vector<std::string_view> arguments;
    arguments.reserve(command_line.size());
    for (std::size_t index = 1; index < command_line.size(); ++index) {
        arguments.emplace_back(command_line[index]);
    }

    const core::Result<application::ExtractOptions> options =
        application::ParseExtractCommandLine(arguments);
    if (!options) {
        Report(options.Failure().Message());
        std::cerr << application::ExtractUsage();
        return Usage;
    }

    if (options.Value().help) {
        std::cout << application::ExtractUsage();
        return Success;
    }

    return Run(options.Value());
}

}  // namespace

int main(int argc, char** argv) {
    // Nothing below here throws as a matter of course: failures are a
    // Result. A std::bad_alloc on a two gigabyte image is the case this
    // catches, and it deserves a sentence rather than a terminate.
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        Report(thrown.what());
        return Failure;
    } catch (...) {
        Report("something was thrown that is not an exception");
        return Failure;
    }
}
