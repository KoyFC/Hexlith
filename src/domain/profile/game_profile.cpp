#include "domain/profile/game_profile.h"

#include <format>

namespace hexlith::domain::profile {

std::string WorkspaceDirectoryName(const GameProfile& profile) {
    return std::format("{:016X}-v{:02}", profile.program_id, profile.remaster_version);
}

}  // namespace hexlith::domain::profile
