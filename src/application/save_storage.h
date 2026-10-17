#ifndef HEXLITH_APPLICATION_SAVE_STORAGE_H
#define HEXLITH_APPLICATION_SAVE_STORAGE_H

#include "core/result.h"
#include "domain/services/save_data.h"
#include "ports/tree_store.h"

#include <optional>
#include <string>
#include <string_view>

namespace hexlith::application {

// The title's save data and extra data kept between runs, as ADR 0015
// describes: one tree for the save data, named savedata, and one for each
// extra data archive, named extdata/<media>-<high>-<low> in fixed-width
// hexadecimal. Each tree's description is the format it was given, as
// key = value lines.
//
// An unformatted save data archive is not stored, so a store that has never
// seen a commit restores to a first boot.

// The name a tree is stored under.
std::string SaveDataTreeName();
std::string ExtraDataTreeName(const domain::services::ExtraDataId& id);
std::optional<domain::services::ExtraDataId> ExtraDataIdFromTreeName(std::string_view name);

// Puts back everything the store holds. A tree whose name is neither kind is
// an error rather than something skipped, since it means the directory holds
// something this program did not write.
core::Status RestoreSaves(ports::TreeStore& store,
                          domain::services::SaveData& save,
                          domain::services::ExtraData& extra);

core::Status StoreSaves(ports::TreeStore& store,
                        const domain::services::SaveData& save,
                        const domain::services::ExtraData& extra);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_SAVE_STORAGE_H
