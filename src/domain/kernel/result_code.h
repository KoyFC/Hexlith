#ifndef HEXLITH_DOMAIN_KERNEL_RESULT_CODE_H
#define HEXLITH_DOMAIN_KERNEL_RESULT_CODE_H

#include <cstdint>

namespace hexlith::domain::kernel {

// The 32-bit result a Horizon system call or service command answers with, in
// r0 or in the first word after the command header. Zero is success, and bit
// 31 marks an error.
//
// A result is guest-visible data, not a Hexlith error: the game reads it and
// decides what to do, so the exact value matters, and every code below is the
// one the console returns. Layout: description in bits 0 to 9, module in 10 to
// 17, summary in 21 to 26, level in 27 to 31. Source: 3dbrew, Error codes; the
// values are Azahar's, src/core/hle/kernel/errors.h, which notes that the
// kernel itself is not consistent about which code it returns where.
struct ResultCode {
    std::uint32_t raw = 0;

    [[nodiscard]] constexpr bool IsError() const {
        return (raw >> ErrorBit) != 0;
    }

    friend constexpr bool operator==(ResultCode, ResultCode) = default;

    static constexpr unsigned ErrorBit = 31;
};

namespace result {

constexpr ResultCode Success{0};

constexpr ResultCode InvalidCombination{0xE0E01BEE};
constexpr ResultCode MisalignedAddress{0xE0E01BF1};
constexpr ResultCode MisalignedSize{0xE0E01BF2};
constexpr ResultCode InvalidAddress{0xE0E01BF5};
constexpr ResultCode InvalidAddressState{0xE0A01BF5};
constexpr ResultCode OutOfRange{0xE0E01BFD};

// Out of heap or linear memory when committing it.
constexpr ResultCode OutOfHeapMemory{0xC860180A};
constexpr ResultCode OutOfMemory{0xD86007F3};
constexpr ResultCode OutOfHandles{0xD8600413};

constexpr ResultCode InvalidEnumValue{0xD8E007ED};
constexpr ResultCode InvalidPointer{0xD8E007F6};
constexpr ResultCode InvalidHandle{0xD8E007F7};
constexpr ResultCode NotFound{0xD88007FA};

// A wait that ran out of time. Not an error: bit 31 is clear.
constexpr ResultCode Timeout{0x09401BFE};

}  // namespace result
}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_RESULT_CODE_H
