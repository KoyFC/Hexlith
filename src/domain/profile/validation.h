#ifndef HEXLITH_DOMAIN_PROFILE_VALIDATION_H
#define HEXLITH_DOMAIN_PROFILE_VALIDATION_H

#include "core/sha256.h"
#include "domain/ctr/exheader.h"
#include "domain/ctr/ncch.h"
#include "domain/ctr/ncsd.h"
#include "domain/profile/game_profile.h"

#include <string>
#include <vector>

namespace hexlith::domain::profile {

// What an image reports about itself, gathered from the three container
// headers so that validation needs no access to the image.
struct ImageIdentity {
    std::uint64_t program_id = 0;
    std::string product_code;
    std::uint16_t maker_code = 0;
    std::uint16_t ncch_format_version = 0;
    std::uint16_t remaster_version = 0;
    std::string application_title;
    SegmentLayout text;
    SegmentLayout read_only;
    SegmentLayout data;
    std::uint32_t stack_size = 0;
    std::uint32_t bss_size = 0;
    std::vector<std::string> services;
    core::Digest exheader_digest;
};

ImageIdentity IdentityOf(const ctr::NcchHeader& ncch, const ctr::ExHeader& exheader);

// Every way the image disagrees with the profile, as sentences.
//
// All of them rather than the first, because the useful question when an
// image is refused is which parts matched. One mismatch is a different
// version; a dozen is a different game.
std::vector<std::string> Mismatches(const GameProfile& profile, const ImageIdentity& identity);

}  // namespace hexlith::domain::profile

#endif  // HEXLITH_DOMAIN_PROFILE_VALIDATION_H
