#include "domain/profile/registry.h"

#include "domain/profile/pokemon_x.h"

#include <array>

namespace hexlith::domain::profile {

std::span<const GameProfile* const> KnownProfiles() {
    // Adding a title means one entry here and one translation unit beside
    // it. Nothing else in the project changes, which is the property
    // ADR 0005 was after.
    //
    // The list is a local static rather than a namespace-scope object, so
    // that it cannot take part in initialisation order across translation
    // units. A registry that can is one where a profile goes missing for
    // reasons nobody can see from the code.
    static const std::array<const GameProfile*, 1> Profiles = {&PokemonX()};
    return Profiles;
}

const GameProfile* FindProfile(std::uint64_t program_id, std::uint16_t remaster_version) {
    for (const GameProfile* profile : KnownProfiles()) {
        if (profile->program_id == program_id && profile->remaster_version == remaster_version) {
            return profile;
        }
    }
    return nullptr;
}

}  // namespace hexlith::domain::profile
