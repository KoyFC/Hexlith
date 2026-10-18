#ifndef HEXLITH_DOMAIN_GPU_COMMAND_PROCESSOR_H
#define HEXLITH_DOMAIN_GPU_COMMAND_PROCESSOR_H

#include "core/result.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"

#include <concepts>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

namespace hexlith::domain::gpu {

// What a command processor hands on. The reference rasterizer, the Vulkan
// backend and the comparison tools each are one; it is a template parameter
// so that nothing between a command word and a draw is a virtual call (ADR
// 0017).
template<typename T>
concept PicaExecutor = requires(T executor, const PicaState& state, bool indexed) {
    // GPUREG_ATTR_BUF_DRAWARRAYS, or DRAWELEMENTS when indexed.
    executor.Draw(state, indexed);
    // A vertex given whole in immediate mode, in state.ImmediateVertex().
    executor.ImmediateVertex(state);
    // The primitive assembler starts again.
    executor.RestartPrimitive(state);
    // The list raised its FINALIZE interrupt.
    executor.Finalize();
};

// The interrupt registers a list cannot write, which the CPU sets before it
// submits one: the compare value of GPUREG_IRQ_CMP(0) and GPUREG_IRQ_AUTOSTOP.
// The defaults are what GBATEK says Nintendo's code sets, 12345678h and
// autostop. A recording does not hold the game's own writes to them.
struct InterruptSetup {
    static constexpr std::uint32_t NintendoCompare = 0x12345678;
    // What Nintendo's code puts in GPUREG_IRQ_MASK: the first four of the 64
    // interrupts enabled.
    static constexpr std::uint32_t NintendoMask = 0xFFFFFFF0;

    std::uint32_t compare = NintendoCompare;
    bool autostop = true;
};

// What a run went through, for the comparison tools and for a person.
struct CommandListRun {
    std::uint64_t writes = 0;
    std::uint64_t jumps = 0;
    bool finalized = false;
};

// Walks command lists, writing each register through the PicaState and
// handing what a write asks for to the executor. Source for the format:
// GBATEK, 3DS GPU Internal Registers - Command Lists. docs/pica-commands.md
// has where Azahar reads it differently.
template<PicaExecutor Executor>
class CommandProcessor {
public:
    CommandProcessor(PicaState& state,
                     const GpuMemory& memory,
                     Executor& executor,
                     InterruptSetup interrupts = {})
        : state_(&state), memory_(&memory), executor_(&executor), interrupts_(interrupts) {}

    // Runs a list given as its bytes, and every list it jumps to.
    core::Result<CommandListRun> Run(std::span<const std::uint8_t> bytes) {
        CommandListRun run;
        std::vector<std::uint32_t> words = Words(bytes);
        for (;;) {
            const std::optional<WriteEffect> stopped_by = Walk(words, run);
            if (!stopped_by.has_value() || *stopped_by == WriteEffect::Finalize) {
                return run;
            }
            if (run.jumps >= MaximumJumps) {
                return core::Error{std::format("a list jumped {} times without ending", run.jumps)};
            }
            ++run.jumps;
            const unsigned channel = *stopped_by == WriteEffect::Jump0 ? 0 : 1;
            const std::span<const std::uint8_t> next = ListAt(channel);
            if (next.empty() && JumpSize(channel) != 0) {
                return core::Error{std::format("a list jumped to {:08X}, outside GPU memory",
                                               JumpAddress(channel))};
            }
            words = Words(next);
        }
    }

private:
    // A list that jumps on and on would never return; the reference title's
    // longest chain is far shorter.
    static constexpr std::uint64_t MaximumJumps = 1U << 20U;

    // GPUREG_CMDBUF_ADDR and SIZE count in units of eight bytes. Source:
    // GBATEK, which describes bit 0 as unused and the rest in 16-byte units;
    // that is the same value shifted by three, and Azahar reads it so.
    static constexpr unsigned CommandBufferUnitShift = 3;
    static constexpr std::uint32_t CommandBufferAddressMask = 0x0FFFFFFF;
    static constexpr std::uint32_t CommandBufferSizeMask = 0x000FFFFF;

    // The command header: the parameter count in bits 20 to 27, bits 28 to
    // 30 unused. Source: GBATEK; Azahar reads the same.
    static constexpr std::uint32_t RegisterIdMask = 0xFFFF;
    static constexpr unsigned ByteMaskShift = 16;
    static constexpr std::uint32_t ByteMaskBits = 0xF;
    static constexpr unsigned ExtraShift = 20;
    static constexpr std::uint32_t ExtraMask = 0xFF;
    static constexpr unsigned ConsecutiveBit = 31;

    // A list's little-endian words.
    static std::vector<std::uint32_t> Words(std::span<const std::uint8_t> bytes) {
        constexpr std::size_t WordBytes = 4;
        constexpr unsigned ByteBits = 8;
        std::vector<std::uint32_t> words(bytes.size() / WordBytes);
        for (std::size_t index = 0; index < words.size(); ++index) {
            for (std::size_t byte = 0; byte < WordBytes; ++byte) {
                words[index] |= static_cast<std::uint32_t>(bytes[(index * WordBytes) + byte])
                                << (ByteBits * byte);
            }
        }
        return words;
    }

    [[nodiscard]] std::uint32_t JumpAddress(unsigned channel) const {
        return (state_->Register(static_cast<std::uint16_t>(reg::CommandBufferAddress0 + channel)) &
                CommandBufferAddressMask)
               << CommandBufferUnitShift;
    }

    [[nodiscard]] std::uint32_t JumpSize(unsigned channel) const {
        return (state_->Register(static_cast<std::uint16_t>(reg::CommandBufferSize0 + channel)) &
                CommandBufferSizeMask)
               << CommandBufferUnitShift;
    }

    [[nodiscard]] std::span<const std::uint8_t> ListAt(unsigned channel) const {
        return memory_->Bytes(JumpAddress(channel), JumpSize(channel));
    }

    // Writes one register and passes on what the write asks for. Answers the
    // effect that ends the list, if this one does.
    std::optional<WriteEffect>
    Write(std::uint16_t id, std::uint32_t value, std::uint32_t byte_mask, CommandListRun& run) {
        ++run.writes;
        const WriteEffect effect = state_->Write(id, value, byte_mask);
        switch (effect) {
        case WriteEffect::None:
            return std::nullopt;
        case WriteEffect::DrawArrays:
        case WriteEffect::DrawElements:
            executor_->Draw(*state_, effect == WriteEffect::DrawElements);
            return std::nullopt;
        case WriteEffect::ImmediateVertex:
            executor_->ImmediateVertex(*state_);
            return std::nullopt;
        case WriteEffect::PrimitiveRestart:
            executor_->RestartPrimitive(*state_);
            return std::nullopt;
        case WriteEffect::Finalize:
            return Finalize(value, run);
        case WriteEffect::Jump0:
        case WriteEffect::Jump1:
            return effect;
        }
        return std::nullopt;
    }

    // FINALIZE raises the interrupt when any byte of the request matches the
    // same byte of the compare value, and with autostop ends the list there.
    // Source: GBATEK, Finalize Interrupt registers.
    std::optional<WriteEffect> Finalize(std::uint32_t request, CommandListRun& run) {
        constexpr std::uint32_t Byte = 0xFF;
        constexpr unsigned ByteBits = 8;
        constexpr unsigned WordBits = 32;
        bool matched = false;
        for (unsigned shift = 0; shift < WordBits; shift += ByteBits) {
            matched =
                matched || ((request >> shift) & Byte) == ((interrupts_.compare >> shift) & Byte);
        }
        if (!matched) {
            return std::nullopt;
        }
        executor_->Finalize();
        run.finalized = true;
        return interrupts_.autostop ? std::optional{WriteEffect::Finalize} : std::nullopt;
    }

    // Walks one list's words to its end, or to a write that ends it. A
    // command runs to its last parameter even when one of its writes is a
    // jump, and the jump is taken once it is done; a FINALIZE that stops the
    // list stops it at once. A command cut short by the end of the list
    // writes what it has.
    std::optional<WriteEffect> Walk(const std::vector<std::uint32_t>& words, CommandListRun& run) {
        std::size_t at = 0;
        while (at + 1 < words.size()) {
            const std::uint32_t header = words[at + 1];
            const auto id = static_cast<std::uint16_t>(header & RegisterIdMask);
            const std::uint32_t byte_mask = (header >> ByteMaskShift) & ByteMaskBits;
            const std::uint32_t extra = (header >> ExtraShift) & ExtraMask;
            const bool consecutive = ((header >> ConsecutiveBit) & 1U) != 0;

            std::optional<WriteEffect> jump;
            for (std::uint32_t parameter = 0; parameter <= extra; ++parameter) {
                // The first parameter comes before the header, the rest after.
                const std::size_t word = parameter == 0 ? at : at + 1 + parameter;
                if (word >= words.size()) {
                    break;
                }
                const auto target = static_cast<std::uint16_t>(consecutive ? id + parameter : id);
                const std::optional<WriteEffect> ended = Write(target, words[word], byte_mask, run);
                if (ended == WriteEffect::Finalize) {
                    return ended;
                }
                if (ended.has_value()) {
                    jump = ended;
                }
            }
            if (jump.has_value()) {
                return jump;
            }
            // Every command takes a whole number of eight-byte units.
            const std::size_t length = 2 + std::size_t{extra};
            at += length + (length % 2);
        }
        return std::nullopt;
    }

    PicaState* state_;
    const GpuMemory* memory_;
    Executor* executor_;
    InterruptSetup interrupts_;
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_COMMAND_PROCESSOR_H
