#ifndef HEXLITH_DOMAIN_SERVICES_RELOCATABLE_OBJECTS_H
#define HEXLITH_DOMAIN_SERVICES_RELOCATABLE_OBJECTS_H

#include "domain/kernel/kernel.h"
#include "domain/linker/cro_linker.h"
#include "domain/linker/module_image.h"
#include "domain/services/command_table.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// ldr:ro, the loader of CRO modules. The game reads a module out of its RomFS
// into a buffer of its own and asks the service to map it at an address it
// chooses, relocate it and link it with the static module and with every
// module already loaded. Commands: 3dbrew, RO Services; answers: RO in Azahar,
// src/core/hle/service/ldr_ro/ldr_ro.cpp.
//
// The words the loader relocates come from linker::Linker, the same
// computation the offline tools make and that was checked against the memory
// of a running game. linker::RebaseModuleImage and FixModuleImage do the rest
// of what the console's loader does to a module's own bytes, and this service
// does what it does to the address space: map the buffer at the module's
// address, writing through the mapping whatever the game may not, keep the
// module list in the headers, and give back the part of the buffer a fixed
// module no longer needs.
//
// docs/cro-linking.md has the order of the steps. LoadCRO_New, unloading and
// linking or unlinking by hand are not implemented yet: the reference title's
// boot reaches none of them before its first module runs.
class RelocatableObjects final : public CommandTable<RelocatableObjects> {
public:
    explicit RelocatableObjects(kernel::Kernel& kernel);

    [[nodiscard]] std::string_view Name() const override {
        return "ldr:ro";
    }

    static std::span<const Command> Commands();

    // Results. Source: ldr_ro.cpp in Azahar, which gives each value.
    static constexpr kernel::ResultCode AlreadyInitialized{0xD9612FF9};
    static constexpr kernel::ResultCode NotInitialized{0xD9612FF8};
    static constexpr kernel::ResultCode BufferTooSmall{0xE0E12C1F};
    static constexpr kernel::ResultCode MisalignedAddress{0xD9012FF1};
    static constexpr kernel::ResultCode MisalignedSize{0xD9012FF2};
    static constexpr kernel::ResultCode IllegalAddress{0xE1612C0F};
    static constexpr kernel::ResultCode InvalidMemoryState{0xD8A12C08};

    // A module the game has loaded or unloaded, where it was mapped.
    struct ModuleEvent {
        enum class Kind : std::uint8_t { Loaded, Unloaded };
        Kind kind = Kind::Loaded;
        std::string_view name;
        std::uint32_t address = 0;
    };

    // Called once a module is mapped and linked, before the reply.
    void OnModuleEvent(std::function<void(const ModuleEvent&)> observer) {
        module_observer_ = std::move(observer);
    }

    // The modules linked so far, the static one first.
    [[nodiscard]] const linker::Linker* Linked() const {
        return linker_.get();
    }

private:
    // A module the service has mapped: where it is, what was read from it,
    // and the external words last written for it, so that a later load writes
    // only the words it changes.
    struct Resident {
        std::unique_ptr<linker::CroModule> module;
        std::uint32_t address = 0;
        std::uint32_t buffer = 0;
        std::uint32_t size = 0;
        std::uint32_t external_table = 0;
        std::vector<std::optional<std::uint32_t>> written;
        std::vector<bool> resolved;
    };

    // What LoadCRO is asked, in the order of its request. Source: 3dbrew,
    // RO:LoadExeCRO.
    struct LoadRequest {
        std::uint32_t buffer = 0;
        std::uint32_t address = 0;
        std::uint32_t size = 0;
        std::uint32_t data_address = 0;
        std::uint32_t zero = 0;
        std::uint32_t data_size = 0;
        std::uint32_t bss_address = 0;
        std::uint32_t bss_size = 0;
        bool auto_link = false;
        std::uint32_t fix_level = 0;
    };

    void Initialize(kernel::IpcRequest& request);
    void LoadCrr(kernel::IpcRequest& request);
    void LoadCro(kernel::IpcRequest& request);

    // The checks Initialize and LoadCRO share on the buffer and the address
    // the module is to be mapped at.
    [[nodiscard]] kernel::ResultCode
    CheckMapping(std::uint32_t buffer, std::uint32_t address, std::uint32_t size) const;

    // Maps the buffer at the address, reads the module out of it and rebases
    // its image. Declines the request and answers nothing if the module cannot
    // be read.
    std::optional<Resident> MapModule(kernel::IpcRequest& request,
                                      std::uint32_t buffer,
                                      std::uint32_t address,
                                      std::uint32_t size,
                                      const linker::Placement& placement,
                                      const linker::BufferSizes& sizes);

    // Writes what the last link changed: the new module's internal words, and
    // every external word and batch flag of every module that now differs
    // from what was written before.
    void WriteLinked(std::size_t newest);
    void WriteInternal(const Resident& resident, const linker::LoadedModule& loaded);

    // Register: the new module goes at the tail of the list whose head the
    // static module's header points at, the auto-linked list or the other one.
    void Register(std::uint32_t address, bool auto_link);
    [[nodiscard]] std::uint32_t HeaderField(std::uint32_t base, std::size_t field) const;
    void SetHeaderField(std::uint32_t base, std::size_t field, std::uint32_t value);

    // Never null; the kernel outlives its services.
    kernel::Kernel* kernel_;
    // Null until Initialize has linked the static module.
    std::unique_ptr<linker::Linker> linker_;
    std::vector<Resident> residents_;
    std::function<void(const ModuleEvent&)> module_observer_;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_RELOCATABLE_OBJECTS_H
