#include "adapters/lifted/lifted_library.h"

#include "application/lift_emit.h"
#include "domain/lift/runtime.h"

#include <bit>
#include <format>

#include <dlfcn.h>

namespace hexlith::adapters::lifted {
namespace {

using ManifestFunction = const char* (*)();
using RegisterFunction = void (*)(void*);

// The symbol as a function pointer. dlsym answers an object pointer and the
// language has no plain cast from one to the other; POSIX requires the two to be
// the same size, which the bit cast checks.
template<class Function>
Function Symbol(void* handle, std::string_view name) {
    const void* const address = dlsym(handle, std::string{name}.c_str());
    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast): the one cast POSIX defines for dlsym.
    return std::bit_cast<Function>(address);
}

}  // namespace

LiftedLibrary::LiftedLibrary(void* handle)
    : handle_(handle), entries_(std::make_unique<domain::lift::runtime::EntryTable>()) {}

const domain::lift::runtime::EntryTable& LiftedLibrary::Entries() const {
    return *entries_;
}

core::Result<std::unique_ptr<LiftedLibrary>>
LiftedLibrary::Open(const std::filesystem::path& path, std::string_view expected_code_digest) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return core::Error{std::format("{} could not be opened: {}", path.string(), dlerror())};
    }
    std::unique_ptr<LiftedLibrary> library{new LiftedLibrary(handle)};

    const auto manifest = Symbol<ManifestFunction>(handle, application::ManifestSymbol);
    const auto fill = Symbol<RegisterFunction>(handle, application::RegisterSymbol);
    if (manifest == nullptr || fill == nullptr) {
        return core::Error{std::format("{} is not a lifted library", path.string())};
    }
    library->manifest_ = manifest();

    const std::string expected =
        std::format("lifter={} code={}", application::LifterVersion, expected_code_digest);
    if (!library->manifest_.starts_with(expected)) {
        return core::Error{std::format(
            "{} was lifted from other code or by another lifter: it says '{}' and this run needs "
            "'{}'",
            path.string(),
            library->manifest_,
            expected)};
    }

    fill(library->entries_.get());
    return library;
}

LiftedLibrary::~LiftedLibrary() {
    if (handle_ != nullptr) {
        dlclose(handle_);
    }
}

}  // namespace hexlith::adapters::lifted
