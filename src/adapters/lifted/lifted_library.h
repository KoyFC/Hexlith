#ifndef HEXLITH_ADAPTERS_LIFTED_LIFTED_LIBRARY_H
#define HEXLITH_ADAPTERS_LIFTED_LIFTED_LIBRARY_H

#include "core/result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace hexlith::domain::lift::runtime {
class EntryTable;
}  // namespace hexlith::domain::lift::runtime

namespace hexlith::adapters::lifted {

// The shared library hexlith-lift --emit's output builds into, opened and read.
//
// The library is derived from the game and lives in the workspace, so the
// repository's own binaries never contain it: they load it by path when asked.
// What it says it was lifted from is compared with what is about to run, because
// lifted code against the wrong code is not a crash that announces itself.
class LiftedLibrary {
public:
    // Opens the library, refuses it unless it was written by this version of the
    // lifter from code with the given SHA-256, and fills the table of entries.
    static core::Result<std::unique_ptr<LiftedLibrary>> Open(const std::filesystem::path& path,
                                                             std::string_view expected_code_digest);

    LiftedLibrary(const LiftedLibrary&) = delete;
    LiftedLibrary& operator=(const LiftedLibrary&) = delete;
    LiftedLibrary(LiftedLibrary&&) = delete;
    LiftedLibrary& operator=(LiftedLibrary&&) = delete;
    ~LiftedLibrary();

    [[nodiscard]] const domain::lift::runtime::EntryTable& Entries() const;

    // What the library says it holds, as its manifest function answered.
    [[nodiscard]] const std::string& Manifest() const {
        return manifest_;
    }

private:
    explicit LiftedLibrary(void* handle);

    void* handle_;
    std::string manifest_;
    std::unique_ptr<domain::lift::runtime::EntryTable> entries_;
};

}  // namespace hexlith::adapters::lifted

#endif  // HEXLITH_ADAPTERS_LIFTED_LIFTED_LIBRARY_H
