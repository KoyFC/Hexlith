#ifndef HEXLITH_ADAPTERS_FILESYSTEM_CARTRIDGE_IMAGE_H
#define HEXLITH_ADAPTERS_FILESYSTEM_CARTRIDGE_IMAGE_H

#include "core/result.h"

#include <filesystem>

namespace hexlith::adapters::filesystem {

// Finds the cartridge image in a directory.
//
// Exactly one candidate is accepted. Two means the user has to say which,
// and none means the directory is not the one they thought, and both are
// better answered than guessed at: picking the first of several is how a tool
// extracts the wrong game and says nothing.
core::Result<std::filesystem::path> FindCartridgeImage(const std::filesystem::path& directory);

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_CARTRIDGE_IMAGE_H
