#ifndef HEXLITH_DOMAIN_PROFILE_REGISTRY_H
#define HEXLITH_DOMAIN_PROFILE_REGISTRY_H

#include "domain/profile/game_profile.h"

#include <cstdint>
#include <span>

namespace hexlith::domain::profile {

// Every profile the build knows, from one explicit list.
//
// ADR 0005 rules out static constructors for this: a registry populated by
// them depends on initialisation order across translation units, which is a
// failure that appears as a profile mysteriously missing. One list in one
// place cannot do that.
std::span<const GameProfile* const> KnownProfiles();

// The profile for a title and version, or nullptr when the build has none.
//
// Returning nothing is the point. An image that matches no profile is
// refused, with the identity it reported, rather than extracted on the
// assumption that one profile is close enough.
const GameProfile* FindProfile(std::uint64_t program_id, std::uint16_t remaster_version);

}  // namespace hexlith::domain::profile

#endif  // HEXLITH_DOMAIN_PROFILE_REGISTRY_H
