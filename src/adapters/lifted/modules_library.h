#ifndef HEXLITH_ADAPTERS_LIFTED_MODULES_LIBRARY_H
#define HEXLITH_ADAPTERS_LIFTED_MODULES_LIBRARY_H

#include "core/result.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hexlith::domain::lift::runtime {
class EntryTable;
class LoadedModules;
}  // namespace hexlith::domain::lift::runtime

namespace hexlith::adapters::lifted {

// The shared library of the CRO modules hexlith-lift --emit-modules writes, once
// built, opened and read.
//
// It holds the lifted code of every module, each with the digest of the code it
// was lifted from. Nothing here is compared with the game: which module is
// loaded where is the loader's to say, and a module whose code is not the code
// that was lifted runs interpreted (domain/lift/runtime.h, LoadedModules). The
// one thing refused at the door is a library from another lifter, whose tables
// mean something else.
class ModulesLibrary {
public:
    static core::Result<std::unique_ptr<ModulesLibrary>> Open(const std::filesystem::path& path);

    ModulesLibrary(const ModulesLibrary&) = delete;
    ModulesLibrary& operator=(const ModulesLibrary&) = delete;
    ModulesLibrary(ModulesLibrary&&) = delete;
    ModulesLibrary& operator=(ModulesLibrary&&) = delete;
    ~ModulesLibrary();

    // Makes the library's modules available to a run. Each run keeps its own
    // record of which are loaded, so it offers to a LoadedModules of its own,
    // which must not outlive this.
    void OfferTo(domain::lift::runtime::LoadedModules& modules) const;

    [[nodiscard]] const std::string& Manifest() const {
        return manifest_;
    }

    [[nodiscard]] std::size_t Count() const {
        return offers_.size();
    }

private:
    struct Offer {
        std::string name;
        std::string digest;
        const domain::lift::runtime::EntryTable* table = nullptr;
    };

    explicit ModulesLibrary(void* handle) : handle_(handle) {}

    void* handle_;
    std::string manifest_;
    std::vector<Offer> offers_;
};

}  // namespace hexlith::adapters::lifted

#endif  // HEXLITH_ADAPTERS_LIFTED_MODULES_LIBRARY_H
