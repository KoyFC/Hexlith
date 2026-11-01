#include "domain/lift/evaluate.h"

#include <optional>

namespace hexlith::domain::lift {
namespace {

namespace arm = domain::arm;
namespace primitives = arm::primitives;
using ir::Operand;
using ir::OpKind;

// More than any instruction lowers to; a lowering that needs more is wrong.
constexpr std::size_t MaximumTemporaries = 64;

bool& FlagOf(arm::CpuState& state, std::uint32_t index) {
    switch (static_cast<ir::Flag>(index)) {
    case ir::Flag::Negative:
        return state.negative;
    case ir::Flag::Zero:
        return state.zero;
    case ir::Flag::Carry:
        return state.carry;
    case ir::Flag::Overflow:
        return state.overflow;
    case ir::Flag::Saturated:
        return state.saturated;
    }
    return state.negative;
}

class Temporaries {
public:
    [[nodiscard]] std::uint32_t Get(const Operand& operand) const {
        switch (operand.kind) {
        case Operand::Kind::Immediate:
            return operand.value;
        case Operand::Kind::Temporary:
            return values_.at(operand.value);
        case Operand::Kind::None:
            break;
        }
        return 0;
    }

    [[nodiscard]] bool Bit(const Operand& operand) const {
        return Get(operand) != 0;
    }

    void Set(std::uint32_t id, std::uint32_t value) {
        if (id != ir::NoTemporary) {
            values_.at(id) = value;
        }
    }

private:
    std::array<std::uint32_t, MaximumTemporaries> values_{};
};

// The result of an operation that may leave the instruction: nothing to say
// when it did not.
using Early = std::optional<Evaluation>;

constexpr Evaluation BailedOut{.kind = EvaluationKind::Bailed, .stop = {}};

std::uint32_t Bit(bool value) {
    return value ? 1U : 0U;
}

// Operations on the processor's state.
void ApplyState(const ir::Op& op, Temporaries& t, arm::CpuState& state) {
    const std::uint32_t a = t.Get(op.a);
    switch (op.kind) {
    case OpKind::ReadRegister:
        t.Set(op.destination, state.registers.at(op.index));
        break;
    case OpKind::WriteRegister:
        state.registers.at(op.index) = a;
        break;
    case OpKind::ReadFlag:
        t.Set(op.destination, Bit(FlagOf(state, op.index)));
        break;
    case OpKind::WriteFlag:
        FlagOf(state, op.index) = a != 0;
        break;
    case OpKind::WriteThumb:
        state.thumb = a != 0;
        break;
    case OpKind::ReadThreadId:
        t.Set(op.destination,
              static_cast<ir::ThreadId>(op.index) == ir::ThreadId::ReadWrite
                  ? state.thread_id_read_write
                  : state.thread_id_read_only);
        break;
    case OpKind::WriteThreadId:
        state.thread_id_read_write = a;
        break;
    default:
        break;
    }
}

// Arithmetic and comparisons, which touch nothing but temporaries.
void ApplyArithmetic(const ir::Op& op, Temporaries& t) {
    const std::uint32_t a = t.Get(op.a);
    const std::uint32_t b = t.Get(op.b);
    switch (op.kind) {
    case OpKind::Add:
        t.Set(op.destination, a + b);
        break;
    case OpKind::Subtract:
        t.Set(op.destination, a - b);
        break;
    case OpKind::And:
        t.Set(op.destination, a & b);
        break;
    case OpKind::Or:
        t.Set(op.destination, a | b);
        break;
    case OpKind::Xor:
        t.Set(op.destination, a ^ b);
        break;
    case OpKind::Multiply:
        t.Set(op.destination, a * b);
        break;
    case OpKind::Equal:
        t.Set(op.destination, Bit(a == b));
        break;
    case OpKind::NotEqual:
        t.Set(op.destination, Bit(a != b));
        break;
    case OpKind::Negative:
        t.Set(op.destination, Bit((a >> 31U) != 0));
        break;
    case OpKind::Select:
        t.Set(op.destination, a != 0 ? b : t.Get(op.c));
        break;
    default:
        break;
    }
}

// The operations that are a call to one of the shared primitives.
void ApplyPrimitive(const ir::Op& op, Temporaries& t) {
    const std::uint32_t a = t.Get(op.a);
    const std::uint32_t b = t.Get(op.b);
    switch (op.kind) {
    case OpKind::Shift: {
        const primitives::ShiftResult shifted =
            primitives::Shift(static_cast<arm::ShiftKind>(op.index), a, b, t.Bit(op.c));
        t.Set(op.destination, shifted.value);
        t.Set(op.destination2, Bit(shifted.carry));
        break;
    }
    case OpKind::Alu: {
        const primitives::AluResult result =
            primitives::ComputeAlu(static_cast<arm::Operation>(op.index),
                                   a,
                                   primitives::ShiftResult{.value = b, .carry = t.Bit(op.c)},
                                   t.Bit(op.d));
        t.Set(op.destination, result.value);
        t.Set(op.destination2, Bit(result.carry));
        t.Set(op.destination3, Bit(result.overflow));
        break;
    }
    case OpKind::ConditionPassed: {
        const primitives::ConditionFlags flags{.negative = t.Bit(op.a),
                                               .zero = t.Bit(op.b),
                                               .carry = t.Bit(op.c),
                                               .overflow = t.Bit(op.d)};
        t.Set(op.destination,
              Bit(primitives::ConditionPassed(static_cast<arm::Condition>(op.index), flags)));
        break;
    }
    default:
        break;
    }
}

template<memory::GuestMemory Memory>
Early ApplyMemory(const ir::Op& op, Temporaries& t, Memory& memory) {
    const std::uint32_t address = t.Get(op.a);
    const auto width = static_cast<primitives::Width>(op.index);
    if (op.kind == OpKind::Load) {
        const std::optional<std::uint32_t> value = primitives::LoadValue(memory, address, width);
        if (!value.has_value()) {
            return BailedOut;
        }
        t.Set(op.destination, value.value());
        return std::nullopt;
    }
    return primitives::StoreValue(memory, address, t.Get(op.b), width) ? std::nullopt
                                                                       : Early{BailedOut};
}

bool PassesItsCondition(const ir::Instruction& instruction, const arm::CpuState& state) {
    if (instruction.condition == arm::Condition::Always) {
        return true;
    }
    const primitives::ConditionFlags flags{.negative = state.negative,
                                           .zero = state.zero,
                                           .carry = state.carry,
                                           .overflow = state.overflow};
    return primitives::ConditionPassed(instruction.condition, flags);
}

template<memory::GuestMemory Memory>
Early Apply(const ir::Instruction& instruction,
            const ir::Op& op,
            Temporaries& t,
            arm::CpuState& state,
            Memory& memory) {
    switch (op.kind) {
    case OpKind::ReadRegister:
    case OpKind::WriteRegister:
    case OpKind::ReadFlag:
    case OpKind::WriteFlag:
    case OpKind::WriteThumb:
    case OpKind::ReadThreadId:
    case OpKind::WriteThreadId:
        ApplyState(op, t, state);
        return std::nullopt;
    case OpKind::Shift:
    case OpKind::Alu:
    case OpKind::ConditionPassed:
        ApplyPrimitive(op, t);
        return std::nullopt;
    case OpKind::Load:
    case OpKind::Store:
        return ApplyMemory(op, t, memory);
    case OpKind::BailIf:
        return t.Bit(op.a) ? Early{BailedOut} : std::nullopt;
    case OpKind::Interpret: {
        state.registers.at(arm::ProgramCounter) = instruction.address;
        const arm::Stop stop = arm::Step(state, memory);
        return stop.Stopped() ? Early{Evaluation{.kind = EvaluationKind::Stopped, .stop = stop}}
                              : std::nullopt;
    }
    default:
        ApplyArithmetic(op, t);
        return std::nullopt;
    }
}

}  // namespace

template<memory::GuestMemory Memory>
Evaluation Evaluate(const ir::Instruction& instruction, arm::CpuState& state, Memory& memory) {
    if (!PassesItsCondition(instruction, state)) {
        return {};
    }

    Temporaries t;
    for (const ir::Op& op : instruction.ops) {
        if (const Early left = Apply(instruction, op, t, state, memory)) {
            return *left;
        }
    }
    return {};
}

template Evaluation
Evaluate<memory::PagedMemory>(const ir::Instruction&, arm::CpuState&, memory::PagedMemory&);
template Evaluation
Evaluate<memory::ProcessMemory>(const ir::Instruction&, arm::CpuState&, memory::ProcessMemory&);

}  // namespace hexlith::domain::lift
