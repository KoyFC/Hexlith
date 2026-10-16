#ifndef HEXLITH_DOMAIN_KERNEL_HANDLE_TABLE_H
#define HEXLITH_DOMAIN_KERNEL_HANDLE_TABLE_H

#include "domain/kernel/objects.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hexlith::domain::kernel {

using Handle = std::uint32_t;

// Two handles every thread may use without having been given them, which the
// kernel resolves to the calling thread and its process. Source: 3dbrew, SVC.
constexpr Handle CurrentThreadHandle = 0xFFFF8000;
constexpr Handle CurrentProcessHandle = 0xFFFF8001;

// A process's handles. A handle is a slot index in its upper bits and a
// generation in its low 15, so that a handle to a closed object stays invalid
// after its slot is reused. Generations start at 1 and skip 0, so no handle is
// zero. Source: HandleTable in Azahar, src/core/hle/kernel/handle_table.cpp.
//
// The values reach the game, and it compares and stores them, so the encoding
// is kept rather than any simpler numbering.
class HandleTable {
public:
    explicit HandleTable(std::size_t capacity);

    // A new handle to the object, or nothing when every slot is in use.
    [[nodiscard]] std::optional<Handle> Create(std::shared_ptr<Object> object);

    // Closes a handle. False when it is not open.
    bool Close(Handle handle);

    // The object a handle refers to, or null for a handle that is not open.
    [[nodiscard]] std::shared_ptr<Object> Get(Handle handle) const;

    // The object if it is of the given type, or null.
    template<typename Kind>
    [[nodiscard]] std::shared_ptr<Kind> Get(Handle handle, ObjectType type) const {
        const std::shared_ptr<Object> object = Get(handle);
        if (object == nullptr || object->Type() != type) {
            return nullptr;
        }
        return std::static_pointer_cast<Kind>(object);
    }

    [[nodiscard]] std::size_t OpenCount() const {
        return open_;
    }

private:
    static constexpr unsigned SlotShift = 15;
    static constexpr std::uint32_t GenerationMask = (1U << SlotShift) - 1;

    struct Slot {
        std::shared_ptr<Object> object;
        std::uint16_t generation = 0;
    };

    std::vector<Slot> slots_;
    std::vector<std::size_t> free_;
    std::uint16_t next_generation_ = 1;
    std::size_t open_ = 0;
};

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_HANDLE_TABLE_H
