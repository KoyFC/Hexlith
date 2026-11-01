#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/workspace_locator.h"
#include "application/command_line.h"
#include "application/lift_survey.h"
#include "application/workspace.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// hexlith-lift wires the adapters to the lifter's use cases and does nothing
// else. Argument parsing lives in the application layer so that it has tests.
namespace {

namespace adapters = hexlith::adapters::filesystem;
namespace application = hexlith::application;
namespace core = hexlith::core;
namespace profile = hexlith::domain::profile;

constexpr int Success = 0;
constexpr int Failure = 1;
constexpr int Usage = 2;

// Where the extraction put the code, and where the analysis put its tables.
constexpr std::string_view CodePath = "exefs/.code";
constexpr std::string_view AnalysisDirectory = "analysis";

void Report(std::string_view message) {
    std::cerr << "hexlith-lift: " << message << '\n';
}

const profile::GameProfile* OnlyProfile() {
    if (profile::KnownProfiles().size() != 1) {
        return nullptr;
    }
    return profile::KnownProfiles().front();
}

core::Result<std::string> ReadText(const std::filesystem::path& path) {
    const std::ifstream file{path};
    if (!file) {
        return core::Error{
            std::format("{} could not be read; run hexlith-analyze first", path.string())};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

core::Result<std::filesystem::path> Extraction(const application::LiftOptions& options,
                                               const profile::GameProfile& chosen) {
    if (options.extraction.has_value()) {
        return std::filesystem::path{*options.extraction};
    }
    // The work directory alone: this tool reads what an extraction and the
    // analysis produced and never looks at the cartridge.
    const core::Result<application::ResolvedPath> work =
        adapters::LocateWorkDirectory(adapters::WorkspaceArguments{
            .rom_directory = std::nullopt, .work_directory = options.work_directory});
    if (!work) {
        return work.Failure();
    }
    return work.Value().path / profile::WorkspaceDirectoryName(chosen);
}

int Survey(const std::filesystem::path& extraction, const profile::GameProfile& chosen) {
    adapters::FileRomSource code{extraction / CodePath};
    const core::Result<core::ByteSpan> code_bytes = code.Contents();
    if (!code_bytes) {
        Report(code_bytes.Failure().WithContext(code.Describe()).Message());
        return Failure;
    }

    const std::filesystem::path tables = extraction / AnalysisDirectory;
    const core::Result<std::string> functions = ReadText(tables / "functions.txt");
    const core::Result<std::string> regions = ReadText(tables / "regions.txt");
    if (!functions || !regions) {
        Report((!functions ? functions : regions).Failure().Message());
        return Failure;
    }

    const core::Result<application::LiftSurvey> survey = application::SurveyControlFlow(
        application::LiftSurveyRequest{.code = code_bytes.Value(),
                                       .base = chosen.text.address,
                                       .functions = functions.Value(),
                                       .regions = regions.Value()});
    if (!survey) {
        Report(survey.Failure().Message());
        return Failure;
    }
    std::cout << application::RenderLiftSurvey(survey.Value());
    return Success;
}

int Run(const application::LiftOptions& options) {
    const profile::GameProfile* chosen = OnlyProfile();
    if (chosen == nullptr) {
        Report("the build knows no single profile to lift");
        return Failure;
    }
    const core::Result<std::filesystem::path> extraction = Extraction(options, *chosen);
    if (!extraction) {
        Report(extraction.Failure().Message());
        return Failure;
    }
    if (!std::filesystem::is_directory(extraction.Value())) {
        Report(std::format("{} is not a directory; run hexlith-extract first",
                           extraction.Value().string()));
        return Failure;
    }
    std::cout << std::format(
        "extraction  {}\nprofile     {}\n\n", extraction.Value().string(), chosen->name);

    if (options.survey) {
        return Survey(extraction.Value(), *chosen);
    }
    Report("nothing to do; --survey is the only mode so far");
    return Usage;
}

int Main(std::span<char* const> arguments) {
    std::vector<std::string_view> words;
    words.reserve(arguments.size());
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        words.emplace_back(arguments[index]);
    }

    const core::Result<application::LiftOptions> options = application::ParseLiftCommandLine(words);
    if (!options) {
        Report(options.Failure().Message());
        std::cerr << '\n' << application::LiftUsage();
        return Usage;
    }
    if (options.Value().help) {
        std::cout << application::LiftUsage();
        return Success;
    }
    return Run(options.Value());
}

}  // namespace

int main(int argc, char** argv) {
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
