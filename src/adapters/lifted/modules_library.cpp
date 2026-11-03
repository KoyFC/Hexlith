#include "adapters/lifted/modules_library.h"

#include "application/lift_emit.h"
#include "domain/lift/runtime.h"

#include <bit>
#include <format>

#include <dlfcn.h>

namespace hexlith::adapters::lifted {
namespace {

using ManifestFunction = const char* (*)();
using OfferFunction = void (*)(void*, const char*, const char*, void*);
using RegisterFunction = void (*)(OfferFunction, void*);

// See LiftedLibrary: the one cast POSIX defines for dlsym.
template<class Function>
Function Symbol(void* handle, std::string_view name) {
    const void* const address = dlsym(handle, std::string{name}.c_str());
    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast): the one cast POSIX defines for dlsym.
    return std::bit_cast<Function>(address);
}

}  // namespace

core::Result<std::unique_ptr<ModulesLibrary>>
ModulesLibrary::Open(const std::filesystem::path& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return core::Error{std::format("{} could not be opened: {}", path.string(), dlerror())};
    }
    std::unique_ptr<ModulesLibrary> library{new ModulesLibrary(handle)};

    const auto manifest = Symbol<ManifestFunction>(handle, application::ModulesManifestSymbol);
    const auto fill = Symbol<RegisterFunction>(handle, application::ModulesRegisterSymbol);
    if (manifest == nullptr || fill == nullptr) {
        return core::Error{std::format("{} is not a library of lifted modules", path.string())};
    }
    library->manifest_ = manifest();

    const std::string expected = std::format("lifter={} ", application::LifterVersion);
    if (!library->manifest_.starts_with(expected)) {
        return core::Error{std::format(
            "{} was lifted by another lifter: it says '{}' and this one is '{}'; lift and build "
            "the modules again",
            path.string(),
            library->manifest_,
            expected)};
    }

    fill(
        [](void* context, const char* name, const char* digest, void* table) {
            static_cast<ModulesLibrary*>(context)->offers_.push_back(
                Offer{.name = name,
                      .digest = digest,
                      .table = static_cast<const domain::lift::runtime::EntryTable*>(table)});
        },
        library.get());
    return library;
}

void ModulesLibrary::OfferTo(domain::lift::runtime::LoadedModules& modules) const {
    for (const Offer& offer : offers_) {
        modules.Offer(offer.name, offer.digest, *offer.table);
    }
}

ModulesLibrary::~ModulesLibrary() {
    if (handle_ != nullptr) {
        dlclose(handle_);
    }
}

}  // namespace hexlith::adapters::lifted
