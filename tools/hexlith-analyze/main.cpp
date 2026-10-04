#include "adapters/filesystem/directory_blob_sink.h"
#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/workspace_locator.h"
#include "application/analyze.h"
#include "application/analyze_modules.h"
#include "application/command_line.h"
#include "application/workspace.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include <algorithm>
#include <exception>
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

// hexlith-analyze wires the adapters to the analyse use case and does nothing
// else. Argument parsing lives in the application layer so that it has tests.
namespace {

namespace adapters = hexlith::adapters::filesystem;
namespace application = hexlith::application;
namespace core = hexlith::core;
namespace profile = hexlith::domain::profile;

constexpr int Success = 0;
constexpr int Failure = 1;
constexpr int Usage = 2;

// Where the extraction put the two files this reads, and where the tables go.
constexpr std::string_view CodePath = "exefs/.code";
constexpr std::string_view StaticModulePath = "romfs/static.crs";
constexpr std::string_view AnalysisDirectory = "analysis";
constexpr std::string_view RomFsDirectory = "romfs";
constexpr std::string_view ModuleExtension = ".cro";
constexpr std::string_view ModulesReportName = "modules/summary.txt";

void Report(std::string_view message) {
    std::cerr << "hexlith-analyze: " << message << '\n';
}

const profile::GameProfile* OnlyProfile() {
    if (profile::KnownProfiles().size() != 1) {
        return nullptr;
    }
    return profile::KnownProfiles().front();
}

// A table that is already there, so that a second run preserves what a person
// put in it. A missing file is a first run and not a failure.
std::string ReadExistingTable(const std::filesystem::path& directory, std::string_view name) {
    std::ifstream file{directory / name};
    if (!file) {
        return {};
    }

    std::ostringstream text;
    text << file.rdbuf();
    file.close();
    return text.str();
}

application::ExistingTables ReadExistingTables(const std::filesystem::path& directory) {
    return application::ExistingTables{
        .symbols = ReadExistingTable(directory, "symbols.txt"),
        .functions = ReadExistingTable(directory, "functions.txt"),
        .regions = ReadExistingTable(directory, "regions.txt"),
        .pointers = ReadExistingTable(directory, "pointers.txt"),
    };
}

// Every loadable module in the RomFS, in name order so that the report reads
// the same from one run to the next.
std::vector<std::filesystem::path> ModuleFiles(const std::filesystem::path& romfs) {
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{romfs}) {
        if (entry.is_regular_file() && entry.path().extension() == ModuleExtension) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

core::Result<core::ByteSpan> ReadFile(adapters::FileRomSource& source);

int AnalyzeTheModules(const std::filesystem::path& extraction,
                      const std::filesystem::path& output,
                      core::ByteSpan static_module,
                      adapters::DirectoryBlobSink& writing) {
    std::vector<std::unique_ptr<adapters::FileRomSource>> sources;
    std::vector<core::ByteSpan> modules;
    application::ExistingModuleTables existing;
    for (const std::filesystem::path& file : ModuleFiles(extraction / RomFsDirectory)) {
        sources.push_back(std::make_unique<adapters::FileRomSource>(file));
        const core::Result<core::ByteSpan> bytes = ReadFile(*sources.back());
        if (!bytes) {
            Report(bytes.Failure().Message());
            return Failure;
        }
        modules.push_back(bytes.Value());

        // The tables of a module are kept under its file name, which in the
        // reference title is always its module name.
        const std::string name = file.stem().string();
        existing.emplace(name, ReadExistingTables(output / application::ModulesDirectory / name));
    }

    const core::Result<application::AnalyzeModulesReport> report = application::AnalyzeModules(
        application::AnalyzeModulesRequest{.static_module = static_module, .modules = modules},
        existing,
        writing);
    if (!report) {
        Report(report.Failure().Message());
        return Failure;
    }

    const core::Status wrote =
        writing.WriteText(ModulesReportName, application::RenderModulesReport(report.Value()));
    if (!wrote) {
        Report(wrote.Failure().Message());
        return Failure;
    }

    std::cout << '\n' << application::RenderModulesSummary(report.Value());
    return Success;
}

core::Result<core::ByteSpan> ReadFile(adapters::FileRomSource& source) {
    const core::Result<core::ByteSpan> contents = source.Contents();
    if (!contents) {
        return contents.Failure().WithContext(source.Describe());
    }
    return contents.Value();
}

int Run(const application::AnalyzeOptions& options) {
    const profile::GameProfile* chosen = OnlyProfile();
    if (chosen == nullptr) {
        Report("the build knows no single profile to analyse against");
        return Failure;
    }

    std::filesystem::path extraction;
    if (options.extraction.has_value()) {
        extraction = *options.extraction;
    } else {
        // The work directory alone. This tool reads what an extraction produced
        // and never looks at the cartridge, so asking for the ROM directory would
        // make it fail over a path it has no use for.
        const core::Result<application::ResolvedPath> work =
            adapters::LocateWorkDirectory(adapters::WorkspaceArguments{
                .rom_directory = std::nullopt, .work_directory = options.work_directory});
        if (!work) {
            Report(work.Failure().Message());
            return Failure;
        }
        extraction = work.Value().path / profile::WorkspaceDirectoryName(*chosen);
    }

    if (!std::filesystem::is_directory(extraction)) {
        Report(
            std::format("{} is not a directory; run hexlith-extract first", extraction.string()));
        return Failure;
    }

    const std::filesystem::path output = options.output.has_value()
                                             ? std::filesystem::path{*options.output}
                                             : extraction / AnalysisDirectory;

    adapters::FileRomSource code{extraction / CodePath};
    adapters::FileRomSource static_module{extraction / StaticModulePath};

    const core::Result<core::ByteSpan> code_bytes = ReadFile(code);
    if (!code_bytes) {
        Report(code_bytes.Failure().Message());
        return Failure;
    }
    const core::Result<core::ByteSpan> module_bytes = ReadFile(static_module);
    if (!module_bytes) {
        Report(module_bytes.Failure().Message());
        return Failure;
    }

    adapters::DirectoryBlobSink writing{output};
    const core::Status prepared = writing.Prepare();
    if (!prepared) {
        Report(prepared.Failure().Message());
        return Failure;
    }

    std::cout << std::format("extraction  {}\n", extraction.string());
    std::cout << std::format("profile     {}\n", chosen->name);
    std::cout << std::format("writing to  {}\n\n", output.string());

    const application::ExistingTables existing = ReadExistingTables(output);

    const application::AnalyzeRequest request{.code = code_bytes.Value(),
                                              .static_module = module_bytes.Value(),
                                              .profile = chosen,
                                              .sweep_data_for_entry_points = !options.no_sweep};

    const core::Result<application::AnalyzeReport> report =
        application::Analyze(request, existing, writing);
    if (!report) {
        Report(report.Failure().Message());
        return Failure;
    }

    std::cout << application::RenderAnalyzeSummary(report.Value());
    return AnalyzeTheModules(extraction, output, module_bytes.Value(), writing);
}

int Main(std::span<char* const> arguments) {
    std::vector<std::string_view> words;
    words.reserve(arguments.size());
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        words.emplace_back(arguments[index]);
    }

    const core::Result<application::AnalyzeOptions> options =
        application::ParseAnalyzeCommandLine(words);
    if (!options) {
        Report(options.Failure().Message());
        std::cerr << '\n' << application::AnalyzeUsage();
        return Usage;
    }

    if (options.Value().help) {
        std::cout << application::AnalyzeUsage();
        return Success;
    }

    return Run(options.Value());
}

}  // namespace

int main(int argc, char** argv) {
    // Nothing below here throws as a matter of course: failures are a Result. A
    // std::bad_alloc on a five megabyte image is the case this catches, and it
    // deserves a sentence rather than a terminate.
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
