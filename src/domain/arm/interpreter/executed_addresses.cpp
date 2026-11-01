#include "domain/arm/interpreter/executed_addresses.h"

#include <algorithm>

namespace hexlith::domain::arm {

ExecutedAddresses::Page& ExecutedAddresses::PageOf(std::uint32_t address) {
    const std::uint32_t number = address >> PageBits;
    if (last_page_ != nullptr && number == last_page_number_) {
        return *last_page_;
    }
    std::unique_ptr<Page>& slot = pages_[number];
    if (slot == nullptr) {
        slot = std::make_unique<Page>();
    }
    last_page_number_ = number;
    last_page_ = slot.get();
    return *last_page_;
}

void ExecutedAddresses::BeforeInstruction(std::uint32_t address, bool thumb) {
    Page& page = PageOf(address);
    const std::uint32_t offset = address & PageMask;
    if (thumb) {
        ++page.thumb.at(offset / 2);
    } else {
        ++page.arm.at(offset / 4);
    }
    ++total_;
}

std::vector<ExecutedAddresses::Entry> ExecutedAddresses::Entries() const {
    std::vector<Entry> entries;
    for (const auto& [number, page] : pages_) {
        const std::uint32_t base = number << PageBits;
        for (std::size_t index = 0; index < page->arm.size(); ++index) {
            if (page->arm.at(index) != 0) {
                entries.push_back({.address = base + static_cast<std::uint32_t>(index * 4),
                                   .thumb = false,
                                   .count = page->arm.at(index)});
            }
        }
        for (std::size_t index = 0; index < page->thumb.size(); ++index) {
            if (page->thumb.at(index) != 0) {
                entries.push_back({.address = base + static_cast<std::uint32_t>(index * 2),
                                   .thumb = true,
                                   .count = page->thumb.at(index)});
            }
        }
    }
    std::ranges::sort(entries, [](const Entry& left, const Entry& right) {
        return left.address != right.address ? left.address < right.address
                                             : left.thumb < right.thumb;
    });
    return entries;
}

std::uint64_t ExecutedAddresses::CountOf(std::uint32_t address, bool thumb) const {
    const auto found = pages_.find(address >> PageBits);
    if (found == pages_.end()) {
        return 0;
    }
    const std::uint32_t offset = address & PageMask;
    return thumb ? found->second->thumb.at(offset / 2) : found->second->arm.at(offset / 4);
}

}  // namespace hexlith::domain::arm
