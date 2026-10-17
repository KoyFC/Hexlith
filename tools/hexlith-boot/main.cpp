#include "adapters/filesystem/cartridge_image.h"
#include "adapters/filesystem/directory_tree_store.h"
#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/gpu_recording_directory.h"
#include "adapters/filesystem/scripted_input.h"
#include "adapters/filesystem/workspace_locator.h"
#include "application/boot.h"
#include "application/command_line.h"
#include "application/golden_trace.h"
#include "application/gpu_recording.h"
#include "application/save_storage.h"
#include "application/workspace.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-boot runs the title from its entry point under the HLE kernel and
// services until something stops it, and says what stopped it: a system call
// or a service command nothing answers yet, a fault, the end of its budget.
// That is how the next thing to implement is found, and with --compare how the
// memory the run leaves is checked against a capture from Azahar.
//
// The wiring is application::BootedTitle; argument parsing is in the
// application layer, where it has tests.
namespace {

namespace adapters = hexlith::adapters::filesystem;
namespace application = hexlith::application;
namespace core = hexlith::core;
namespace kernel = hexlith::domain::kernel;
namespace profile = hexlith::domain::profile;

constexpr int Success = 0;
constexpr int Failure = 1;
constexpr int Usage = 2;

constexpr double Million = 1e6;

constexpr std::string_view CaptureExtension = ".bin";

// Where the title's save data and GPU recording are kept, under its directory
// in the workspace.
constexpr std::string_view SaveDirectory = "save";
constexpr std::string_view GpuDirectory = "gpu";

void PrintRecording(const application::GpuRecordingSummary& summary, std::string_view where) {
    constexpr std::array<std::string_view, 6> Names = {
        "DMA", "command list", "memory fill", "display transfer", "texture copy", "cache flush"};
    std::cout << std::format("gpu     {}\n", where);
    for (std::size_t id = 0; id < Names.size(); ++id) {
        std::cout << std::format("        {:>8} {}\n", summary.commands.at(id), Names.at(id));
    }
    std::cout << std::format("        {} lists of {} to {} bytes, {} in all; {} pages\n",
                             summary.lists,
                             summary.smallest_list,
                             summary.largest_list,
                             summary.list_bytes,
                             summary.pages);
}

void Report(std::string_view message) {
    std::cerr << "hexlith-boot: " << message << '\n';
}

const profile::GameProfile* OnlyProfile() {
    if (profile::KnownProfiles().size() != 1) {
        return nullptr;
    }
    return profile::KnownProfiles().front();
}

std::string_view Describe(kernel::KernelStopReason reason) {
    switch (reason) {
    case kernel::KernelStopReason::BudgetSpent:
        return "ran out of its instruction budget";
    case kernel::KernelStopReason::UnimplementedSystemCall:
        return "made a system call nothing implements";
    case kernel::KernelStopReason::ProcessorStop:
        return "stopped the processor";
    case kernel::KernelStopReason::Deadlock:
        return "has no thread that can ever run again";
    case kernel::KernelStopReason::ProcessExited:
        return "exited";
    case kernel::KernelStopReason::Break:
        return "called Break";
    case kernel::KernelStopReason::UnimplementedServiceCommand:
        return "sent a command nothing implements";
    case kernel::KernelStopReason::Requested:
        return "was stopped where it was asked to stop";
    }
    return "stopped for a reason this tool does not know";
}

void Trace(kernel::Kernel& running) {
    running.OnServiceRequest([](const kernel::ServiceRequestRecord& record) {
        std::cout << std::format("{:>12} t{} ipc {:<12} {:08X} -> {:08X}{}\n",
                                 record.tick,
                                 record.thread_id,
                                 record.service,
                                 record.header,
                                 record.result,
                                 record.implemented ? "" : " (not implemented)");
    });
    running.OnSystemCall([](const kernel::SystemCallRecord& record) {
        std::cout << std::format("{:>12} t{} svc {:02X} {:<28} at {:08X} -> {:08X}\n",
                                 record.tick,
                                 record.thread_id,
                                 record.number,
                                 kernel::Kernel::SystemCallName(record.number),
                                 record.address,
                                 record.result);
    });
}

void PrintStop(const kernel::KernelStop& stop) {
    std::cout << std::format("\nthe title {}", Describe(stop.reason));
    switch (stop.reason) {
    case kernel::KernelStopReason::UnimplementedServiceCommand:
        std::cout << std::format(": {} {:08X}", stop.service, stop.detail);
        break;
    case kernel::KernelStopReason::UnimplementedSystemCall:
        std::cout << std::format(
            ": {:02X} {}", stop.detail, kernel::Kernel::SystemCallName(stop.detail));
        break;
    case kernel::KernelStopReason::Break:
        std::cout << std::format(", reason {:08X}", stop.detail);
        break;
    default:
        break;
    }
    std::cout << std::format("\nthread {} at {:08X}\n", stop.thread_id, stop.address);
}

void PrintModules(const application::BootedTitle& title) {
    const auto* linked = title.Loader().Linked();
    if (linked == nullptr) {
        std::cout << "no module loaded\n";
        return;
    }
    for (const auto& module : linked->Modules()) {
        if (module.module->header.IsStaticModule()) {
            continue;
        }
        std::cout << std::format("module {} at {:08X}, data {:08X}, bss {:08X}\n",
                                 module.Name(),
                                 module.placement.module_address,
                                 module.placement.data_address,
                                 module.placement.bss_address);
    }
}

std::optional<std::uint32_t> AddressFromName(const std::filesystem::path& file) {
    constexpr int Hexadecimal = 16;
    const std::string stem = file.stem().string();
    try {
        std::size_t used = 0;
        const unsigned long value = std::stoul(stem, &used, Hexadecimal);
        if (used != stem.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Compares every capture in the directory with the process's memory at the
// address its name gives. Answers whether all of them match.
bool Compare(application::BootedTitle& title, const std::filesystem::path& directory) {
    bool all_match = true;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.is_regular_file() && entry.path().extension() == CaptureExtension) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    for (const std::filesystem::path& file : files) {
        const std::optional<std::uint32_t> address = AddressFromName(file);
        if (!address.has_value()) {
            continue;
        }
        std::ifstream stream{file, std::ios::binary};
        const std::vector<std::uint8_t> expected{std::istreambuf_iterator<char>{stream}, {}};
        const application::MemoryComparison comparison =
            application::CompareMemory(title.Kernel().Space().Memory(), *address, expected);
        if (!comparison.readable) {
            std::cout << std::format("compare {:08X}: not mapped\n", *address);
            all_match = false;
            continue;
        }
        std::cout << std::format("compare {:08X}: {} of {} bytes differ",
                                 *address,
                                 comparison.differing,
                                 expected.size());
        if (comparison.first_difference.has_value()) {
            std::cout << std::format(", the first at {:08X}",
                                     *address + *comparison.first_difference);
            all_match = false;
        }
        std::cout << '\n';
    }
    return all_match;
}

core::Result<std::filesystem::path> ChooseImage(const application::BootOptions& options,
                                                const application::Workspace& workspace) {
    if (options.image.has_value()) {
        return std::filesystem::path{*options.image};
    }
    return adapters::FindCartridgeImage(workspace.rom_directory.path);
}

// A save that cannot be written stops nothing: it is reported, and the run
// goes on with the archive in memory.
void StoreSaves(adapters::DirectoryTreeStore& saves, application::BootedTitle& title) {
    const core::Status stored = application::StoreSaves(saves, title.Save(), title.Extra());
    if (!stored) {
        Report(stored.Failure().Message());
    }
}

bool AttachInput(const application::BootOptions& options,
                 application::BootedTitle& title,
                 std::unique_ptr<adapters::ScriptedInput>& input) {
    if (!options.input.has_value()) {
        return true;
    }
    core::Result<std::unique_ptr<adapters::ScriptedInput>> loaded =
        adapters::ScriptedInput::Load(*options.input);
    if (!loaded) {
        Report(loaded.Failure().Message());
        return false;
    }
    input = std::move(loaded.Value());
    title.UseInput(*input);
    std::cout << std::format(
        "input   {}, to frame {}\n", input->Describe(), input->Script().LastFrame());
    return true;
}

bool AttachGolden(const application::BootOptions& options,
                  application::BootedTitle& title,
                  std::ofstream& golden) {
    if (!options.golden.has_value()) {
        return true;
    }
    golden.open(*options.golden, std::ios::trunc);
    if (!golden) {
        Report(std::format("'{}' could not be opened for writing", *options.golden));
        return false;
    }
    application::RecordGoldenTrace(title,
                                   [&golden](std::string_view line) { golden << line << '\n'; });
    return true;
}

void StopAfter(const application::BootStopPoint& point, application::BootedTitle& title) {
    using Event = hexlith::domain::services::RelocatableObjects::ModuleEvent;
    const Event::Kind wanted = point.unload ? Event::Kind::Unloaded : Event::Kind::Loaded;
    title.Loader().OnModuleEvent([point, wanted, &title](const Event& event) {
        if (event.kind == wanted && event.name == point.module) {
            title.Kernel().RequestStop();
        }
    });
}

// The save data outlives the run unless this is a new game.
bool AttachSaves(const application::BootOptions& options,
                 application::BootedTitle& title,
                 adapters::DirectoryTreeStore& saves) {
    if (options.new_game) {
        std::cout << "saves   none, a new game\n";
        return true;
    }
    const core::Status restored = application::RestoreSaves(saves, title.Save(), title.Extra());
    if (!restored) {
        Report(restored.Failure().Message());
        return false;
    }
    std::cout << std::format(
        "saves   {}{}\n", saves.Describe(), title.Save().IsFormatted() ? "" : ", none yet");
    title.Save().OnCommit([&saves, &title](const auto&) { StoreSaves(saves, title); });
    return true;
}

int Run(const application::BootOptions& options) {
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
    std::cout << std::format("image   {}\nprofile {}\n", image.Value().string(), target->name);

    adapters::FileRomSource source{image.Value()};
    const core::Result<core::ByteSpan> contents = source.Contents();
    if (!contents) {
        Report(contents.Failure().Message());
        return Failure;
    }
    core::Result<std::unique_ptr<application::BootedTitle>> booted =
        application::BootedTitle::FromCartridge(contents.Value(), target->executable_partition);
    if (!booted) {
        Report(booted.Failure().Message());
        return Failure;
    }
    application::BootedTitle& title = *booted.Value();
    kernel::Kernel& running = title.Kernel();
    if (options.trace) {
        Trace(running);
    }
    std::unique_ptr<adapters::ScriptedInput> input;
    std::ofstream golden;
    adapters::DirectoryTreeStore saves{workspace.Value().work_directory.path /
                                       profile::WorkspaceDirectoryName(*target) / SaveDirectory};
    if (!AttachInput(options, title, input) || !AttachGolden(options, title, golden) ||
        !AttachSaves(options, title, saves)) {
        return Failure;
    }
    if (options.stop_after.has_value()) {
        StopAfter(*options.stop_after, title);
    }
    adapters::GpuRecordingDirectory gpu{workspace.Value().work_directory.path /
                                        profile::WorkspaceDirectoryName(*target) / GpuDirectory};
    std::unique_ptr<application::GpuRecording> recording;
    if (options.record_gpu) {
        const core::Status prepared = gpu.Prepare();
        if (!prepared) {
            Report(prepared.Failure().Message());
            return Failure;
        }
        recording = std::make_unique<application::GpuRecording>(title, gpu);
    }

    const auto began = std::chrono::steady_clock::now();
    const kernel::KernelStop stop = running.Run(options.instruction_limit);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    PrintStop(stop);
    PrintModules(title);
    if (recording != nullptr) {
        const core::Status finished = recording->Finish();
        if (!finished) {
            Report(finished.Failure().Message());
        }
        PrintRecording(recording->Summary(), gpu.Describe());
    }
    if (!options.new_game) {
        StoreSaves(saves, title);
    }
    std::cout << std::format("{} instructions, {} ticks, in {:.2f} s: {:.1f} million a second\n",
                             running.Executed(),
                             running.Threads().Ticks(),
                             seconds,
                             static_cast<double>(running.Executed()) / seconds / Million);

    if (options.compare.has_value() && !Compare(title, *options.compare)) {
        return Failure;
    }
    return Success;
}

int Main(std::span<char* const> command_line) {
    std::vector<std::string_view> arguments;
    arguments.reserve(command_line.size());
    for (std::size_t index = 1; index < command_line.size(); ++index) {
        arguments.emplace_back(command_line[index]);
    }

    const core::Result<application::BootOptions> options =
        application::ParseBootCommandLine(arguments);
    if (!options) {
        Report(options.Failure().Message());
        std::cerr << application::BootUsage();
        return Usage;
    }
    if (options.Value().help) {
        std::cout << application::BootUsage();
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
