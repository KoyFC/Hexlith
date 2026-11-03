#include "adapters/filesystem/directory_blob_sink.h"
#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/workspace_locator.h"
#include "application/command_line.h"
#include "application/lift_emit.h"
#include "application/lift_survey.h"
#include "application/workspace.h"
#include "domain/ctr/cro.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include "common/tool_main.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
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
constexpr std::string_view LiftDirectory = "lift";
constexpr std::string_view ModulesLiftDirectory = "modules";
constexpr std::string_view RomFsDirectory = "romfs";
constexpr std::string_view ModuleExtension = ".cro";
constexpr std::string_view ModuleTablesDirectory = "modules";

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

int Survey(const std::filesystem::path& extraction,
           const profile::GameProfile& chosen,
           const application::LiftOptions& options) {
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

    // The table of a run is optional, and absent is not a failure.
    const std::filesystem::path executed_path = options.executed.has_value()
                                                    ? std::filesystem::path{*options.executed}
                                                    : extraction / "observed" / "executed.txt";
    std::string executed;
    if (const core::Result<std::string> text = ReadText(executed_path)) {
        executed = text.Value();
    } else if (options.executed.has_value()) {
        Report(text.Failure().Message());
        return Failure;
    }

    const core::Result<application::LiftSurvey> survey = application::SurveyControlFlow(
        application::LiftSurveyRequest{.code = code_bytes.Value(),
                                       .base = chosen.text.address,
                                       .functions = functions.Value(),
                                       .regions = regions.Value(),
                                       .executed = executed});
    if (!survey) {
        Report(survey.Failure().Message());
        return Failure;
    }
    std::cout << application::RenderLiftSurvey(survey.Value());
    return Success;
}

int Emit(const std::filesystem::path& extraction,
         const profile::GameProfile& chosen,
         const application::LiftOptions& options) {
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

    const std::filesystem::path output = options.output.has_value()
                                             ? std::filesystem::path{*options.output}
                                             : extraction / LiftDirectory;
    adapters::DirectoryBlobSink sink{output};
    if (const core::Status prepared = sink.Prepare(); !prepared) {
        Report(prepared.Failure().Message());
        return Failure;
    }
    std::cout << std::format("writing to {}\n", output.string());

    const core::Result<application::LiftEmitReport> report =
        application::EmitLiftedCode(application::LiftEmitRequest{.code = code_bytes.Value(),
                                                                 .base = chosen.text.address,
                                                                 .functions = functions.Value(),
                                                                 .regions = regions.Value()},
                                    sink);
    if (!report) {
        Report(report.Failure().Message());
        return Failure;
    }
    std::cout << application::RenderLiftEmitSummary(report.Value());
    return Success;
}

// A module's file and its analysis tables, kept so that the request can look at
// them.
struct ModuleFiles {
    std::vector<std::unique_ptr<adapters::FileRomSource>> sources;
    std::vector<std::string> names;
    std::vector<core::ByteSpan> files;
    std::vector<std::string> functions;
    std::vector<std::string> regions;
};

// Every loadable module in the RomFS, in name order, with the tables the
// analysis wrote for it. The loader knows a module by the name in its header,
// which is also the directory its tables are in.
core::Result<ModuleFiles> ReadModuleFiles(const std::filesystem::path& extraction) {
    std::vector<std::filesystem::path> paths;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{extraction / RomFsDirectory}) {
        if (entry.is_regular_file() && entry.path().extension() == ModuleExtension) {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths);

    ModuleFiles modules;
    for (const std::filesystem::path& path : paths) {
        modules.sources.push_back(std::make_unique<adapters::FileRomSource>(path));
        const core::Result<core::ByteSpan> bytes = modules.sources.back()->Contents();
        if (!bytes) {
            return bytes.Failure().WithContext(modules.sources.back()->Describe());
        }
        const core::Result<hexlith::domain::ctr::CroHeader> header =
            hexlith::domain::ctr::ParseCroHeader(bytes.Value());
        if (!header) {
            return header.Failure().WithContext(path.string());
        }
        const std::filesystem::path tables =
            extraction / AnalysisDirectory / ModuleTablesDirectory / header.Value().name;
        const core::Result<std::string> functions = ReadText(tables / "functions.txt");
        const core::Result<std::string> regions = ReadText(tables / "regions.txt");
        if (!functions || !regions) {
            return (!functions ? functions : regions).Failure();
        }
        modules.names.push_back(header.Value().name);
        modules.files.push_back(bytes.Value());
        modules.functions.push_back(functions.Value());
        modules.regions.push_back(regions.Value());
    }
    return modules;
}

int EmitModules(const std::filesystem::path& extraction, const application::LiftOptions& options) {
    const core::Result<ModuleFiles> read = ReadModuleFiles(extraction);
    if (!read) {
        Report(read.Failure().Message());
        return Failure;
    }
    const ModuleFiles& modules = read.Value();
    std::vector<application::ModuleSource> sources;
    sources.reserve(modules.names.size());
    for (std::size_t index = 0; index < modules.names.size(); ++index) {
        sources.push_back(application::ModuleSource{.name = modules.names.at(index),
                                                    .file = modules.files.at(index),
                                                    .functions = modules.functions.at(index),
                                                    .regions = modules.regions.at(index)});
    }

    const std::filesystem::path output = options.output.has_value()
                                             ? std::filesystem::path{*options.output}
                                             : extraction / LiftDirectory / ModulesLiftDirectory;
    adapters::DirectoryBlobSink sink{output};
    if (const core::Status prepared = sink.Prepare(); !prepared) {
        Report(prepared.Failure().Message());
        return Failure;
    }
    std::cout << std::format("writing to {}\n", output.string());

    const core::Result<application::LiftModulesReport> report =
        application::EmitLiftedModules(application::LiftModulesRequest{.modules = sources}, sink);
    if (!report) {
        Report(report.Failure().Message());
        return Failure;
    }
    std::cout << application::RenderLiftModulesSummary(report.Value());
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
        return Survey(extraction.Value(), *chosen, options);
    }
    if (options.emit) {
        return Emit(extraction.Value(), *chosen, options);
    }
    if (options.emit_modules) {
        return EmitModules(extraction.Value(), options);
    }
    Report("nothing to do; ask for --survey, --emit or --emit-modules");
    return Usage;
}

}  // namespace

int main(int argc, char** argv) {
    return hexlith::tools::RunCommand(
        "hexlith-lift", argc, argv, application::ParseLiftCommandLine, application::LiftUsage, Run);
}
