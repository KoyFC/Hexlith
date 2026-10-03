#include "domain/profile/validation.h"

#include <format>

namespace hexlith::domain::profile {
namespace {

SegmentLayout LayoutOf(const ctr::CodeSetInfo& segment) {
    return SegmentLayout{
        .address = segment.address, .page_count = segment.page_count, .size = segment.size};
}

template<typename T>
void Compare(std::vector<std::string>& mismatches,
             std::string_view what,
             const T& expected,
             const T& found) {
    if (expected != found) {
        mismatches.push_back(
            std::format("{}: the profile says {} and the image reports {}", what, expected, found));
    }
}

void CompareHex(std::vector<std::string>& mismatches,
                std::string_view what,
                std::uint64_t expected,
                std::uint64_t found) {
    if (expected != found) {
        mismatches.push_back(std::format(
            "{}: the profile says {:#x} and the image reports {:#x}", what, expected, found));
    }
}

void CompareSegment(std::vector<std::string>& mismatches,
                    std::string_view what,
                    const SegmentLayout& expected,
                    const SegmentLayout& found) {
    if (expected.address == found.address && expected.page_count == found.page_count &&
        expected.size == found.size) {
        return;
    }
    mismatches.push_back(
        std::format("the {} segment: the profile says {:#x} across {} page(s) of {} bytes and "
                    "the image reports {:#x} across {} of {}",
                    what,
                    expected.address,
                    expected.page_count,
                    expected.size,
                    found.address,
                    found.page_count,
                    found.size));
}

void CompareServices(std::vector<std::string>& mismatches,
                     std::span<const std::string_view> expected,
                     const std::vector<std::string>& found) {
    if (expected.size() != found.size()) {
        mismatches.push_back(std::format(
            "the service access control list: the profile names {} service(s) and the image {}",
            expected.size(),
            found.size()));
        return;
    }

    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] != found[index]) {
            mismatches.push_back(
                std::format("service {}: the profile says '{}' and the image reports '{}'",
                            index,
                            expected[index],
                            found[index]));
        }
    }
}

}  // namespace

ImageIdentity IdentityOf(const ctr::NcchHeader& ncch, const ctr::ExHeader& exheader) {
    ImageIdentity identity;
    identity.program_id = ncch.program_id;
    identity.product_code = ncch.product_code;
    identity.maker_code = ncch.maker_code;
    identity.ncch_format_version = ncch.format_version;
    identity.remaster_version = exheader.remaster_version;
    identity.application_title = exheader.application_title;
    identity.text = LayoutOf(exheader.text);
    identity.read_only = LayoutOf(exheader.read_only);
    identity.data = LayoutOf(exheader.data);
    identity.stack_size = exheader.stack_size;
    identity.bss_size = exheader.bss_size;
    identity.services = exheader.services;
    identity.exheader_digest = ncch.exheader_hash;
    return identity;
}

std::vector<std::string> Mismatches(const GameProfile& profile, const ImageIdentity& identity) {
    std::vector<std::string> mismatches;

    CompareHex(mismatches, "the program ID", profile.program_id, identity.program_id);
    Compare(
        mismatches, "the product code", std::string{profile.product_code}, identity.product_code);
    CompareHex(mismatches, "the maker code", profile.maker_code, identity.maker_code);
    CompareHex(mismatches,
               "the NCCH format version",
               profile.ncch_format_version,
               identity.ncch_format_version);
    CompareHex(
        mismatches, "the remaster version", profile.remaster_version, identity.remaster_version);
    Compare(mismatches,
            "the application title",
            std::string{profile.application_title},
            identity.application_title);

    CompareSegment(mismatches, "text", profile.text, identity.text);
    CompareSegment(mismatches, "read-only", profile.read_only, identity.read_only);
    CompareSegment(mismatches, "data", profile.data, identity.data);

    CompareHex(mismatches, "the stack size", profile.stack_size, identity.stack_size);
    CompareHex(mismatches, "the BSS size", profile.bss_size, identity.bss_size);

    CompareServices(mismatches, profile.services, identity.services);

    Compare(mismatches,
            "the extended header hash",
            std::string{profile.exheader_digest},
            identity.exheader_digest.ToHex());

    return mismatches;
}

}  // namespace hexlith::domain::profile
