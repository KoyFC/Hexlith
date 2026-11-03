#ifndef HEXLITH_TOOLS_COMMON_LIFTED_MODULES_H
#define HEXLITH_TOOLS_COMMON_LIFTED_MODULES_H

#include "adapters/lifted/modules_library.h"
#include "application/boot.h"
#include "application/lift_emit.h"
#include "core/result.h"
#include "domain/lift/runtime.h"
#include "domain/services/relocatable_objects.h"

#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>

// What the tools that run the lifted library do about the library of modules,
// which sits beside it once built.
namespace hexlith::tools {

// Opens the library of modules beside the lifted one. A library that has not
// been built is not a failure: the modules run interpreted, and the run says so.
// One that is there and cannot be used is, since running without it would pass
// for lifted.
inline core::Result<std::unique_ptr<adapters::lifted::ModulesLibrary>>
OpenModulesBeside(const std::string& lifted_library, std::ostream& out) {
    const std::filesystem::path path = application::ModulesLibraryBeside(lifted_library);
    if (!std::filesystem::exists(path)) {
        out << std::format("modules  none lifted: {} is not built\n", path.string());
        return std::unique_ptr<adapters::lifted::ModulesLibrary>{};
    }
    core::Result<std::unique_ptr<adapters::lifted::ModulesLibrary>> opened =
        adapters::lifted::ModulesLibrary::Open(path);
    if (opened) {
        out << std::format("modules  {}, {}\n", path.string(), opened.Value()->Manifest());
    }
    return opened;
}

// Has the engine find a module's code while the module is loaded. A module that
// runs interpreted is said so once, when it is loaded, and why.
//
// The record of what is loaded belongs to the run, and must outlive the title's
// loader events.
inline void FollowModules(application::BootedTitle& title,
                          domain::lift::runtime::LoadedModules& modules,
                          std::ostream& warnings) {
    using Event = domain::services::RelocatableObjects::ModuleEvent;
    using Outcome = domain::lift::runtime::LoadedModules::Outcome;
    title.Loader().OnModuleEvent([&modules, &warnings](const Event& event) {
        if (event.kind == Event::Kind::Unloaded) {
            modules.Unloaded(event.name, event.address);
            return;
        }
        switch (modules.Loaded(
            event.name, event.address, event.code_offset, event.code_size, event.code_digest)) {
        case Outcome::Lifted:
            break;
        case Outcome::NotLifted:
            warnings << std::format("module {} at {:08X} runs interpreted: nothing was lifted "
                                    "for it\n",
                                    event.name,
                                    event.address);
            break;
        case Outcome::DigestDiffers:
            warnings << std::format("module {} at {:08X} runs interpreted: its code is not the "
                                    "code that was lifted\n",
                                    event.name,
                                    event.address);
            break;
        }
    });
}

}  // namespace hexlith::tools

#endif  // HEXLITH_TOOLS_COMMON_LIFTED_MODULES_H
