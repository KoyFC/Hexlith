#include "adapters/filesystem/cartridge_image.h"
#include "adapters/filesystem/directory_blob_sink.h"
#include "adapters/filesystem/directory_tree_store.h"
#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/gpu_recording_directory.h"
#include "adapters/filesystem/png_frame_sink.h"
#include "adapters/filesystem/scripted_input.h"
#include "adapters/filesystem/workspace_locator.h"
#include "adapters/lifted/lifted_library.h"
#include "adapters/lifted/modules_library.h"
#include "adapters/sdl/sdl_window.h"
#include "adapters/vulkan/vulkan_backend.h"
#include "application/boot.h"
#include "application/command_line.h"
#include "application/executed_table.h"
#include "application/golden_trace.h"
#include "application/gpu_recording.h"
#include "application/gpu_render.h"
#include "application/live_display.h"
#include "application/save_storage.h"
#include "application/workspace.h"
#include "domain/arm/interpreter/executed_addresses.h"
#include "domain/lift/engine.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include "common/lifted_modules.h"
#include "common/tool_main.h"

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
namespace lifted_adapters = hexlith::adapters::lifted;
namespace application = hexlith::application;
namespace core = hexlith::core;
namespace kernel = hexlith::domain::kernel;
namespace profile = hexlith::domain::profile;

constexpr int Success = 0;
constexpr int Failure = 1;

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
    std::cout << std::format("        {} framebuffers shown\n", summary.framebuffers);
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

// What is asked of every vertical blank: stop at the frame --stop-after named,
// stop when the window is closed, and say how long the run has taken at the
// frames --time-frames names, to be read by scripts/measure_window_speed.py.
void WatchVerticalBlanks(const application::BootOptions& options,
                         application::BootedTitle& title,
                         const hexlith::ports::ScreenSink* sink,
                         const std::chrono::steady_clock::time_point& began) {
    using Kind = application::BootStopPoint::Kind;
    const bool at_frame = options.stop_after.has_value() && options.stop_after->kind == Kind::Frame;
    if (!at_frame && sink == nullptr && options.time_frames.empty()) {
        return;
    }
    const std::uint64_t frame_to_stop = at_frame ? options.stop_after->frame : 0;
    title.Gpu().OnVerticalBlank([at_frame, frame_to_stop, sink, &title, &began, &options](
                                    std::uint64_t frame) {
        if (std::ranges::binary_search(options.time_frames, frame)) {
            std::cout << std::format(
                "timing  frame {} wall {:.3f} s guest {:.3f} s instructions {}\n",
                frame,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(),
                std::chrono::duration<double>(title.Kernel().GuestTime()).count(),
                title.Kernel().Executed());
        }
        if ((at_frame && frame == frame_to_stop) || (sink != nullptr && sink->StopRequested())) {
            title.Kernel().RequestStop();
        }
    });
}

void StopAfter(const application::BootStopPoint& point, application::BootedTitle& title) {
    using Kind = application::BootStopPoint::Kind;
    if (point.kind == Kind::Frame) {
        return;
    }
    using Event = hexlith::domain::services::RelocatableObjects::ModuleEvent;
    const Event::Kind wanted =
        point.kind == Kind::ModuleUnload ? Event::Kind::Unloaded : Event::Kind::Loaded;
    title.Loader().OnModuleEvent([point, wanted, &title](const Event& event) {
        if (event.kind == wanted && event.name == point.module) {
            title.Kernel().RequestStop();
        }
    });
}

// What shows the title on screen while it runs: the title's memory as the GPU
// sees it, the backend that draws into it, the window, and the display that
// feeds them. Kept alive for the run, in the order they depend on each other.
struct Display {
    std::unique_ptr<hexlith::domain::gpu::GpuMemory> memory;
    std::unique_ptr<hexlith::adapters::vulkan::VulkanContext> context;
    std::unique_ptr<hexlith::ports::GpuBackend> backend;
    std::unique_ptr<hexlith::adapters::sdl::SdlWindow> window;
    // In front of the window, when --save-frames asks for pictures.
    std::unique_ptr<hexlith::adapters::filesystem::PngFrameSink> pictures;
    std::unique_ptr<hexlith::application::LiveDisplay> display;
};

constexpr int WindowScale = 2;

bool AttachWindow(const application::BootOptions& options,
                  application::BootedTitle& title,
                  Display& shown) {
    namespace gpu = hexlith::domain::gpu;
    namespace vulkan = hexlith::adapters::vulkan;
    if (!options.window) {
        return true;
    }
    shown.memory = std::make_unique<gpu::GpuMemory>(application::GpuMemoryOf(title));
    if (options.vulkan) {
        auto made = vulkan::VulkanContext::Create({.device = options.device, .validation = false});
        if (!made) {
            Report(made.Failure().Message());
            return false;
        }
        shown.context = std::move(made.Value());
        auto backend = vulkan::VulkanGpuBackend::Create(*shown.context,
                                                        *shown.memory,
                                                        gpu::Sampling::Hardware,
                                                        vulkan::VertexShading::GpuWherePossible,
                                                        options.generic_fragments
                                                            ? vulkan::FragmentShading::Generic
                                                            : vulkan::FragmentShading::Specialised);
        if (!backend) {
            Report(backend.Failure().Message());
            return false;
        }
        shown.backend = std::move(backend.Value());
    } else {
        shown.backend = std::make_unique<application::ReferenceGpuBackend>(*shown.memory,
                                                                           gpu::Sampling::Hardware);
    }
    auto window = hexlith::adapters::sdl::SdlWindow::Create("Hexlith", WindowScale);
    if (!window) {
        Report(window.Failure().Message());
        return false;
    }
    shown.window = std::move(window.Value());
    hexlith::ports::ScreenSink* sink = shown.window.get();
    if (options.save_frames_directory.has_value()) {
        shown.pictures = std::make_unique<hexlith::adapters::filesystem::PngFrameSink>(
            *options.save_frames_directory, options.save_frames, shown.window.get());
        sink = shown.pictures.get();
    }
    shown.display =
        std::make_unique<application::LiveDisplay>(*shown.memory, *shown.backend, *sink);
    shown.display->Watch(title.Gpu());
    // A script makes a run repeatable, so it wins over the keyboard.
    if (!options.input.has_value()) {
        title.UseInput(*shown.window);
        std::cout << "input   the keyboard and mouse, in the window\n";
    }
    std::cout << std::format("window  {}\n", shown.display->Describe());
    return true;
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

// The lifted library and the engine that runs it, kept alive for the run.
struct LiftedCode {
    std::unique_ptr<lifted_adapters::LiftedLibrary> library;
    std::unique_ptr<lifted_adapters::ModulesLibrary> modules_library;
    std::unique_ptr<hexlith::domain::lift::runtime::LoadedModules> modules;
    std::unique_ptr<hexlith::domain::lift::Engine> engine;
};

bool AttachLifted(const application::BootOptions& options,
                  application::BootedTitle& title,
                  LiftedCode& lifted) {
    if (!options.lifted.has_value()) {
        return true;
    }
    core::Result<std::unique_ptr<lifted_adapters::LiftedLibrary>> opened =
        lifted_adapters::LiftedLibrary::Open(*options.lifted, title.CodeDigest());
    if (!opened) {
        Report(opened.Failure().Message());
        return false;
    }
    lifted.library = std::move(opened.Value());
    std::cout << std::format("lifted  {}, {}\n", *options.lifted, lifted.library->Manifest());

    core::Result<std::unique_ptr<lifted_adapters::ModulesLibrary>> modules =
        hexlith::tools::OpenModulesBeside(*options.lifted, std::cout);
    if (!modules) {
        Report(modules.Failure().Message());
        return false;
    }
    lifted.modules_library = std::move(modules.Value());
    if (lifted.modules_library != nullptr) {
        lifted.modules = std::make_unique<hexlith::domain::lift::runtime::LoadedModules>();
        lifted.modules_library->OfferTo(*lifted.modules);
        hexlith::tools::FollowModules(title, *lifted.modules, std::cerr);
    }
    lifted.engine = std::make_unique<hexlith::domain::lift::Engine>(lifted.library->Entries(),
                                                                    lifted.modules.get());
    title.Kernel().UseLiftedCode(lifted.engine.get());
    return true;
}

// Has the recorder know which modules are loaded where, to tell their
// instructions from the executable's.
void FollowModulesForRecording(application::BootedTitle& title,
                               application::ExecutionRecorder& recorder) {
    using Event = hexlith::domain::services::RelocatableObjects::ModuleEvent;
    title.Loader().OnModuleEvent([&recorder](const Event& event) {
        if (event.kind == Event::Kind::Loaded) {
            recorder.ModuleLoaded(event.name, event.address, event.code_offset, event.code_size);
        } else {
            recorder.ModuleUnloaded(event.name, event.address);
        }
    });
}

// What the run executed, written beside the other things derived from the title:
// the main executable's, and under modules/ the instructions of each module that
// ran, by offset from where the module was.
core::Status WriteExecuted(const application::Workspace& workspace,
                           const profile::GameProfile& target,
                           application::BootedTitle& title,
                           const application::ExecutionRecorder& recorder) {
    adapters::DirectoryBlobSink sink{workspace.work_directory.path /
                                     profile::WorkspaceDirectoryName(target) /
                                     application::ObservedDirectory};
    if (const core::Status prepared = sink.Prepare(); !prepared) {
        return prepared;
    }
    const auto& text = title.Header().text;
    const hexlith::domain::arm::ExecutedAddresses& executed = recorder.Main();
    const std::string table = application::RenderExecutedTable(
        executed,
        text.address,
        text.address + static_cast<std::uint32_t>(text.PhysicalRegionSize()));
    core::Status written = sink.WriteText(application::ExecutedTableName, table);
    if (!written) {
        return written;
    }
    std::cout << std::format("executed {}/{}, {} instructions\n",
                             sink.Describe(),
                             application::ExecutedTableName,
                             executed.Total());

    std::uint64_t in_modules = 0;
    for (const auto& [name, record] : recorder.Modules()) {
        if (record.executed.Total() == 0) {
            continue;
        }
        const std::string module_table = application::RenderExecutedTable(
            record.executed, record.code_offset, record.code_offset + record.code_size);
        const core::Status module_written =
            sink.WriteText(std::format("{}/{}/{}",
                                       application::ObservedModulesDirectory,
                                       name,
                                       application::ExecutedTableName),
                           module_table);
        if (!module_written) {
            return module_written;
        }
        in_modules += record.executed.Total();
    }
    std::cout << std::format("executed {} instructions in modules\n", in_modules);
    return written;
}

// The title the image holds. It reads its RomFS from the source's bytes, so the
// source has to outlive it.
core::Result<std::unique_ptr<application::BootedTitle>>
BootTitle(adapters::FileRomSource& source, const profile::GameProfile& target) {
    const core::Result<core::ByteSpan> contents = source.Contents();
    if (!contents) {
        return contents.Failure();
    }
    return application::BootedTitle::FromCartridge(contents.Value(), target.executable_partition);
}

bool AttachGpuRecording(const application::BootOptions& options,
                        application::BootedTitle& title,
                        adapters::GpuRecordingDirectory& gpu,
                        std::unique_ptr<application::GpuRecording>& recording) {
    if (!options.record_gpu) {
        return true;
    }
    const core::Status prepared = gpu.Prepare();
    if (!prepared) {
        Report(prepared.Failure().Message());
        return false;
    }
    recording = std::make_unique<application::GpuRecording>(title, gpu);
    return true;
}

std::string Seconds(std::chrono::nanoseconds time) {
    return std::format("{:.3f} s", std::chrono::duration<double>(time).count());
}

// Where the time of drawing went, to be read by scripts/measure_window_speed.py.
// What the backend does not split out of RunCommandList is the command
// processor and the vertices the CPU shades.
void PrintDisplayWork(const Display& shown) {
    const hexlith::application::LiveDisplay::WorkDone& work = shown.display->Work();
    const hexlith::ports::GpuBackendStatistics statistics = shown.backend->Statistics();
    const hexlith::ports::GpuBackendTimes& times = statistics.times;
    std::cout << std::format("display lists {} in {}, transfers {} in {}, present {}\n",
                             work.lists,
                             Seconds(work.lists_time),
                             work.transfers,
                             Seconds(work.transfers_time),
                             Seconds(work.present_time));
    std::cout << std::format(
        "backend {} draws in {} groups, {} bytes up and {} down, {} buffers made, {} conflicts\n",
        statistics.draws,
        statistics.groups,
        statistics.uploaded_bytes,
        statistics.downloaded_bytes,
        statistics.allocations,
        statistics.conflicts);
    std::cout << std::format("        textures {}, up {}, down {}, device {}, buffers {}\n",
                             Seconds(times.textures),
                             Seconds(times.upload),
                             Seconds(times.download),
                             Seconds(times.wait),
                             Seconds(times.allocate));
    std::cout << std::format(
        "        {} fragment configurations, {} pipelines made in {}, {} draws waited for none\n",
        statistics.fragment_configurations,
        statistics.pipelines,
        Seconds(times.pipelines),
        statistics.fallback_draws);
}

// Closes what watched the run: the GPU recording and the window.
void FinishWatching(application::GpuRecording* recording,
                    const adapters::GpuRecordingDirectory& gpu,
                    const Display& shown) {
    if (recording != nullptr) {
        const core::Status finished = recording->Finish();
        if (!finished) {
            Report(finished.Failure().Message());
        }
        PrintRecording(recording->Summary(), gpu.Describe());
    }
    if (shown.display != nullptr) {
        const core::Status finished = shown.display->Finish();
        if (!finished) {
            Report(finished.Failure().Message());
        }
        std::cout << std::format("window  {} frames shown\n", shown.display->Presented());
        PrintDisplayWork(shown);
    }
}

// How fast the run went, overall and in the game's own code, and how much of it
// lifted code left to the interpreter.
void PrintSpeed(const kernel::Kernel& running, double seconds, const LiftedCode& lifted_code) {
    std::cout << std::format("{} instructions, {} ticks, in {:.2f} s: {:.1f} million a second\n",
                             running.Executed(),
                             running.Threads().Ticks(),
                             seconds,
                             static_cast<double>(running.Executed()) / seconds / Million);

    const double guest_seconds = std::chrono::duration<double>(running.GuestTime()).count();
    std::cout << std::format(
        "guest   {:.3f} s running the game's code: {:.1f} million instructions a second\n",
        guest_seconds,
        static_cast<double>(running.Executed()) / guest_seconds / Million);
    if (lifted_code.engine != nullptr) {
        const auto& counted = lifted_code.engine->Counted();
        std::cout << std::format("lifted  {} entries into lifted code, {} of them into modules, "
                                 "{} instructions interpreted ({:.2f}%)\n",
                                 counted.entries,
                                 counted.module_entries,
                                 counted.interpreted,
                                 100.0 * static_cast<double>(counted.interpreted) /
                                     static_cast<double>(running.Executed()));
    }
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
    core::Result<std::unique_ptr<application::BootedTitle>> booted = BootTitle(source, *target);
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
    Display shown;
    if (!AttachWindow(options, title, shown)) {
        return Failure;
    }
    std::chrono::steady_clock::time_point began;
    WatchVerticalBlanks(options, title, shown.window.get(), began);
    adapters::GpuRecordingDirectory gpu{workspace.Value().work_directory.path /
                                        profile::WorkspaceDirectoryName(*target) / GpuDirectory};
    std::unique_ptr<application::GpuRecording> recording;
    if (!AttachGpuRecording(options, title, gpu, recording)) {
        return Failure;
    }

    application::ExecutionRecorder recorder;
    if (options.record_executed) {
        FollowModulesForRecording(title, recorder);
        running.ObserveExecution(&recorder);
    }

    LiftedCode lifted_code;
    if (!AttachLifted(options, title, lifted_code)) {
        return Failure;
    }

    began = std::chrono::steady_clock::now();
    const kernel::KernelStop stop = running.Run(options.instruction_limit);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    PrintStop(stop);
    PrintModules(title);
    FinishWatching(recording.get(), gpu, shown);
    if (options.record_executed) {
        const core::Status written = WriteExecuted(workspace.Value(), *target, title, recorder);
        if (!written) {
            Report(written.Failure().Message());
        }
    }
    if (!options.new_game) {
        StoreSaves(saves, title);
    }
    PrintSpeed(running, seconds, lifted_code);
    if (options.compare.has_value() && !Compare(title, *options.compare)) {
        return Failure;
    }
    return Success;
}

}  // namespace

int main(int argc, char** argv) {
    return hexlith::tools::RunCommand(
        "hexlith-boot", argc, argv, application::ParseBootCommandLine, application::BootUsage, Run);
}
