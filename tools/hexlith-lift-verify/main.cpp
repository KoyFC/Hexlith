#include "adapters/filesystem/cartridge_image.h"
#include "adapters/filesystem/file_rom_source.h"
#include "adapters/filesystem/scripted_input.h"
#include "adapters/filesystem/workspace_locator.h"
#include "adapters/lifted/lifted_library.h"
#include "application/boot.h"
#include "application/command_line.h"
#include "application/lift_verification.h"
#include "application/workspace.h"
#include "domain/lift/engine.h"
#include "domain/profile/game_profile.h"
#include "domain/profile/registry.h"

#include "common/tool_main.h"

#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-lift-verify boots the title twice from a new game, once through the
// interpreter alone and once through the lifted library, and has
// application::VerifyLiftedCode compare the two. It wires the adapters and does
// nothing else; argument parsing is in the application layer, where it has
// tests.
namespace {

namespace adapters = hexlith::adapters::filesystem;
namespace lifted_adapters = hexlith::adapters::lifted;
namespace application = hexlith::application;
namespace core = hexlith::core;
namespace profile = hexlith::domain::profile;

constexpr int Success = 0;
constexpr int Failure = 1;
// Lifted code differed from the interpreter.
constexpr int Differed = 3;

void Report(std::string_view message) {
    std::cerr << "hexlith-lift-verify: " << message << '\n';
}

// A booted title, and for the lifted run the engine that runs its library.
class TitleRun final : public application::VerifiedRun {
public:
    TitleRun(std::unique_ptr<application::BootedTitle> title,
             std::unique_ptr<hexlith::domain::lift::Engine> engine)
        : title_(std::move(title)), engine_(std::move(engine)) {
        if (engine_ != nullptr) {
            title_->Kernel().UseLiftedCode(engine_.get());
        }
    }

    hexlith::domain::kernel::Kernel& Kernel() override {
        return title_->Kernel();
    }

private:
    std::unique_ptr<application::BootedTitle> title_;
    std::unique_ptr<hexlith::domain::lift::Engine> engine_;
};

// What every run is made from, opened once.
struct Inputs {
    core::ByteSpan cartridge;
    std::size_t partition = 0;
    adapters::ScriptedInput* input = nullptr;
    const lifted_adapters::LiftedLibrary* library = nullptr;
};

application::VerifiedRunMaker MakerFor(const Inputs& inputs) {
    return [inputs](bool lifted) -> core::Result<std::unique_ptr<application::VerifiedRun>> {
        core::Result<std::unique_ptr<application::BootedTitle>> booted =
            application::BootedTitle::FromCartridge(inputs.cartridge, inputs.partition);
        if (!booted) {
            return booted.Failure();
        }
        if (inputs.input != nullptr) {
            booted.Value()->UseInput(*inputs.input);
        }
        std::unique_ptr<hexlith::domain::lift::Engine> engine;
        if (lifted) {
            engine = std::make_unique<hexlith::domain::lift::Engine>(inputs.library->Entries());
        }
        return std::unique_ptr<application::VerifiedRun>{
            std::make_unique<TitleRun>(std::move(booted.Value()), std::move(engine))};
    };
}

// The script of the player's input the options name, or none.
core::Result<std::unique_ptr<adapters::ScriptedInput>>
LoadInput(const application::LiftVerifyOptions& options) {
    if (!options.input.has_value()) {
        return std::unique_ptr<adapters::ScriptedInput>{};
    }
    return adapters::ScriptedInput::Load(*options.input);
}

// The lifted library, checked against the title's code digest. A run of the
// title is made only to read the digest from.
core::Result<std::unique_ptr<lifted_adapters::LiftedLibrary>>
OpenLifted(const std::string& path, core::ByteSpan cartridge, const profile::GameProfile& target) {
    const core::Result<std::unique_ptr<application::BootedTitle>> first =
        application::BootedTitle::FromCartridge(cartridge, target.executable_partition);
    if (!first) {
        return first.Failure();
    }
    return lifted_adapters::LiftedLibrary::Open(path, first.Value()->CodeDigest());
}

// Says how far the two runs agreed, and where they parted if they did.
int PrintVerification(const application::LiftVerification& result) {
    if (!result.divergence.has_value()) {
        std::cout << std::format("agreed  after {} instructions{}\n",
                                 result.agreed,
                                 result.stop.has_value() ? ", where both runs stopped alike" : "");
        return Success;
    }
    const application::LiftDivergence& divergence = *result.divergence;
    std::cout << std::format("agreed  after {} instructions, with a thread at {:08X}\n",
                             result.agreed,
                             divergence.last_agreed_pc);
    std::cout << std::format(
        "differ  after {}: {}\n", divergence.instructions, divergence.difference);
    return Differed;
}

int Run(const application::LiftVerifyOptions& options) {
    if (profile::KnownProfiles().size() != 1) {
        Report("this build knows more than one profile and cannot choose between them");
        return Failure;
    }
    const profile::GameProfile& target = *profile::KnownProfiles().front();
    const core::Result<application::Workspace> workspace =
        adapters::LocateWorkspace(adapters::WorkspaceArguments{
            .rom_directory = options.rom_directory, .work_directory = options.work_directory});
    if (!workspace) {
        Report(workspace.Failure().Message());
        return Failure;
    }
    const core::Result<std::filesystem::path> image =
        options.image.has_value()
            ? core::Result<std::filesystem::path>{std::filesystem::path{*options.image}}
            : adapters::FindCartridgeImage(workspace.Value().rom_directory.path);
    if (!image) {
        Report(image.Failure().Message());
        return Failure;
    }
    adapters::FileRomSource source{image.Value()};
    const core::Result<core::ByteSpan> contents = source.Contents();
    if (!contents) {
        Report(contents.Failure().Message());
        return Failure;
    }

    const core::Result<std::unique_ptr<adapters::ScriptedInput>> input = LoadInput(options);
    if (!input) {
        Report(input.Failure().Message());
        return Failure;
    }
    // The parser refuses a command line without it.
    const std::string lifted_path = options.lifted.value_or("");
    const core::Result<std::unique_ptr<lifted_adapters::LiftedLibrary>> library =
        OpenLifted(lifted_path, contents.Value(), target);
    if (!library) {
        Report(library.Failure().Message());
        return Failure;
    }
    std::cout << std::format("lifted  {}, {}\n", lifted_path, library.Value()->Manifest());
    std::cout << std::format(
        "verify  {} instructions, compared every {}\n", options.instruction_limit, options.step);

    const Inputs inputs{.cartridge = contents.Value(),
                        .partition = target.executable_partition,
                        .input = input.Value().get(),
                        .library = library.Value().get()};
    const core::Result<application::LiftVerification> verified =
        application::VerifyLiftedCode(MakerFor(inputs), options.instruction_limit, options.step);
    if (!verified) {
        Report(verified.Failure().Message());
        return Failure;
    }
    return PrintVerification(verified.Value());
}

}  // namespace

int main(int argc, char** argv) {
    return hexlith::tools::RunCommand("hexlith-lift-verify",
                                      argc,
                                      argv,
                                      application::ParseLiftVerifyCommandLine,
                                      application::LiftVerifyUsage,
                                      Run);
}
