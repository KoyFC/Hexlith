#include "domain/kernel/handle_table.h"

#include <utility>

namespace hexlith::domain::kernel {

HandleTable::HandleTable(std::size_t capacity) : slots_(capacity) {
    // Slots are handed out lowest first, as the console's table does, so the
    // first handle a process gets is slot 0 with generation 1.
    for (std::size_t slot = capacity; slot > 0; --slot) {
        free_.push_back(slot - 1);
    }
}

std::optional<Handle> HandleTable::Create(std::shared_ptr<Object> object) {
    if (free_.empty()) {
        return std::nullopt;
    }
    const std::size_t slot = free_.back();
    free_.pop_back();

    const std::uint16_t generation = next_generation_;
    ++next_generation_;
    if (next_generation_ > GenerationMask) {
        next_generation_ = 1;
    }
    slots_.at(slot) = Slot{.object = std::move(object), .generation = generation};
    ++open_;
    return static_cast<Handle>(generation) | static_cast<Handle>(slot << SlotShift);
}

bool HandleTable::Close(Handle handle) {
    const std::size_t slot = handle >> SlotShift;
    if (Get(handle) == nullptr) {
        return false;
    }
    slots_.at(slot) = Slot{};
    free_.push_back(slot);
    --open_;
    return true;
}

std::shared_ptr<Object> HandleTable::Get(Handle handle) const {
    const std::size_t slot = handle >> SlotShift;
    if (slot >= slots_.size()) {
        return nullptr;
    }
    const Slot& entry = slots_.at(slot);
    if (entry.object == nullptr || entry.generation != (handle & GenerationMask)) {
        return nullptr;
    }
    return entry.object;
}

}  // namespace hexlith::domain::kernel
