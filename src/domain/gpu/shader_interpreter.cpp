#include "domain/gpu/shader_interpreter.h"

#include "domain/gpu/shader_arithmetic.h"
#include "domain/gpu/shader_encoding.h"

#include <cmath>

namespace hexlith::domain::gpu {
namespace {

namespace opcode = shader::opcode;
using shader::Field;

// The last word of code memory is taken as END whatever it holds, which is
// what Azahar does to keep a program without one from running off the end.
constexpr std::uint32_t LastInstruction = ShaderUnitState::CodeWords - 1;
constexpr std::size_t Components = 4;

// A stack of fixed depth that drops its oldest entry when pushed full, as
// Azahar's circular buffers do.
template<typename T, std::size_t Depth>
class BoundedStack {
public:
    void Push(const T& entry) {
        if (size_ == Depth) {
            for (std::size_t index = 1; index < Depth; ++index) {
                entries_.at(index - 1) = entries_.at(index);
            }
            --size_;
        }
        entries_.at(size_) = entry;
        ++size_;
    }

    [[nodiscard]] bool Empty() const {
        return size_ == 0;
    }

    [[nodiscard]] std::size_t Size() const {
        return size_;
    }

    T& Top() {
        return entries_.at(size_ - 1);
    }

    void Pop() {
        --size_;
    }

private:
    std::array<T, Depth> entries_{};
    std::size_t size_ = 0;
};

struct IfScope {
    std::uint32_t else_address = 0;
    std::uint32_t end_address = 0;
};

struct CallScope {
    std::uint32_t end_address = 0;
    std::uint32_t return_address = 0;
};

struct LoopScope {
    std::uint32_t entry_address = 0;
    std::uint32_t end_address = 0;
    std::uint8_t remaining = 0;
    std::uint8_t increment = 0;
    std::int32_t previous_counter = 0;
};

// An operand descriptor, applied to the sources it describes.
class Descriptor {
public:
    explicit Descriptor(std::uint32_t word) : word_(word) {}

    [[nodiscard]] bool Writes(std::size_t component) const {
        return shader::DescriptorWrites(word_, component);
    }

    // Source 0, 1 or 2 of the instruction.
    [[nodiscard]] Vec4 Apply(std::size_t source, const Vec4& value) const {
        const bool negated = shader::DescriptorNegates(word_, source);
        Vec4 result{};
        for (std::size_t component = 0; component < Components; ++component) {
            const float selected = value.at(shader::DescriptorSelector(word_, source, component));
            result.at(component) = negated ? -selected : selected;
        }
        return result;
    }

private:
    std::uint32_t word_;
};

class Machine {
public:
    Machine(const ShaderUnitState& unit, ShaderRegisters& registers, const ShaderRun& run)
        : unit_(&unit), registers_(&registers), run_(&run), counter_(run.entry) {}

    ShaderRunResult Run() {
        while (!stopped_) {
            if (result_.instructions >= run_->limit) {
                result_.stopped = true;
                break;
            }
            Step();
        }
        return result_;
    }

private:
    void Step();
    void Execute(std::uint32_t word);
    void Arithmetic(std::uint32_t word, std::uint32_t code);
    void MultiplyAdd(std::uint32_t word);
    void Compare(std::uint32_t word);
    void FlowControl(std::uint32_t word, std::uint32_t code);
    void CloseScopes(std::uint32_t old_counter);

    [[nodiscard]] Vec4 Source(std::uint32_t raw, std::uint32_t relative) const;

    // Source `index` of an instruction, as its descriptor selects and negates it.
    [[nodiscard]] Vec4 Operand(const shader::Operands& operands, std::size_t index) const {
        const shader::Source& source = operands.sources.at(index);
        return Descriptor{unit_->descriptors.at(operands.descriptor)}.Apply(
            index, Source(source.index, source.relative));
    }

    float* Destination(std::uint32_t raw);
    void Write(std::uint32_t raw, const Descriptor& descriptor, const Vec4& value);
    [[nodiscard]] bool Condition(std::uint32_t word) const;
    [[nodiscard]] bool Bool(std::uint32_t word) const;

    void Jump(std::uint32_t target) {
        // The counter steps on after every instruction, so a jump lands one
        // short of its target. It is unsigned, so a target of zero wraps.
        counter_ = target - 1;
    }

    [[nodiscard]] float Round(float value) const {
        return run_->precision == ShaderPrecision::Float24 ? RoundToFloat24(value) : value;
    }

    const ShaderUnitState* unit_;
    ShaderRegisters* registers_;
    const ShaderRun* run_;
    ShaderRunResult result_;
    std::uint32_t counter_;
    bool stopped_ = false;
    bool breaking_ = false;
    // Source: GBATEK, Control Flow.
    BoundedStack<IfScope, 8> ifs_;
    BoundedStack<CallScope, 4> calls_;
    BoundedStack<LoopScope, 4> loops_;
};

void Machine::Step() {
    const std::uint32_t old_counter = counter_;
    breaking_ = false;
    const std::uint32_t word =
        counter_ < LastInstruction ? unit_->code.at(counter_) : opcode::End << 26U;
    Execute(word);
    ++counter_;
    ++result_.instructions;
    CloseScopes(old_counter);
}

void Machine::Execute(std::uint32_t word) {
    const std::uint32_t code = shader::OpcodeOf(word);
    if (shader::IsMultiplyAdd(word)) {
        MultiplyAdd(word);
    } else if (shader::IsCompare(word)) {
        Compare(word);
    } else if (code < opcode::Break) {
        Arithmetic(word, code);
    } else {
        FlowControl(word, code);
    }
}

// A source register: an input below 10h, a temporary below 20h, a float
// uniform from there. Only a uniform can be addressed relative to a0.x, a0.y
// or aL; an offset outside a signed byte counts as zero, the index wraps at
// 80h, and an index past c95 reads ones. Source: GBATEK, Relative
// addressing; Azahar's interpreter for the limits.
Vec4 Machine::Source(std::uint32_t raw, std::uint32_t relative) const {
    constexpr std::uint32_t FirstTemporary = 0x10;
    constexpr std::uint32_t FirstUniform = 0x20;
    constexpr std::int32_t SmallestOffset = -128;
    constexpr std::int32_t LargestOffset = 127;
    constexpr std::uint32_t IndexMask = 0x7F;
    if (raw < FirstTemporary) {
        return registers_->inputs.at(raw);
    }
    if (raw < FirstUniform) {
        return registers_->temporaries.at(raw - FirstTemporary);
    }
    std::uint32_t index = raw - FirstUniform;
    if (relative != 0) {
        std::int32_t offset = registers_->address.at(relative - 1);
        if (offset < SmallestOffset || offset > LargestOffset) {
            offset = 0;
        }
        index = (index + static_cast<std::uint32_t>(offset)) & IndexMask;
        if (index >= ShaderUnitState::FloatUniforms) {
            return Vec4{1.0F, 1.0F, 1.0F, 1.0F};
        }
    }
    return unit_->float_uniforms.at(index);
}

float* Machine::Destination(std::uint32_t raw) {
    constexpr std::uint32_t FirstTemporary = 0x10;
    if (raw < FirstTemporary) {
        return registers_->outputs.at(registers_->output_bank).at(raw).data();
    }
    return registers_->temporaries.at(raw - FirstTemporary).data();
}

void Machine::Write(std::uint32_t raw, const Descriptor& descriptor, const Vec4& value) {
    float* target = Destination(raw);
    for (std::size_t component = 0; component < Components; ++component) {
        if (descriptor.Writes(component)) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): a Vec4's data
            target[component] = Round(value.at(component));
        }
    }
}

Vec4 Splat(float value) {
    return Vec4{value, value, value, value};
}

// One component of the instructions that work a component at a time.
float Componentwise(std::uint32_t code, float a, float b) {
    switch (code) {
    case opcode::Add:
        return a + b;
    case opcode::Mul:
        return ShaderMultiply(a, b);
    case opcode::Max:
        return ShaderMax(a, b);
    case opcode::Min:
        return ShaderMin(a, b);
    case opcode::Sge:
    case opcode::Sgei:
        return a >= b ? 1.0F : 0.0F;
    case opcode::Slt:
    case opcode::Slti:
        return a < b ? 1.0F : 0.0F;
    case opcode::Flr:
        return std::floor(a);
    default:
        return a;
    }
}

void Machine::Arithmetic(std::uint32_t word, std::uint32_t code) {
    const shader::Operands operands = shader::ArithmeticOperands(word);
    const Descriptor descriptor{unit_->descriptors.at(operands.descriptor)};
    const Vec4 first = Operand(operands, 0);
    const Vec4 second = Operand(operands, 1);

    Vec4 result{};
    switch (code) {
    case opcode::Add:
    case opcode::Mul:
    case opcode::Max:
    case opcode::Min:
    case opcode::Sge:
    case opcode::Sgei:
    case opcode::Slt:
    case opcode::Slti:
    case opcode::Flr:
    case opcode::Mov:
        for (std::size_t c = 0; c < Components; ++c) {
            result.at(c) = Componentwise(code, first.at(c), second.at(c));
        }
        break;
    case opcode::Dp3:
    case opcode::Dp4:
    case opcode::Dph:
    case opcode::Dphi: {
        Vec4 left = first;
        if (code == opcode::Dph || code == opcode::Dphi) {
            left.at(3) = 1.0F;
        }
        // From a positive zero, a term at a time, as Azahar sums.
        float sum = 0.0F;
        for (std::size_t c = 0; c < (code == opcode::Dp3 ? 3U : Components); ++c) {
            sum = sum + ShaderMultiply(left.at(c), second.at(c));
        }
        result = Splat(sum);
        break;
    }
    case opcode::Rcp:
        result = Splat(1.0F / first.at(0));
        break;
    case opcode::Rsq:
        result = Splat(1.0F / std::sqrt(first.at(0)));
        break;
    case opcode::Ex2:
        result = Splat(std::exp2(first.at(0)));
        break;
    case opcode::Lg2:
        result = Splat(std::log2(first.at(0)));
        break;
    case opcode::Mova:
        for (std::size_t c = 0; c < 2; ++c) {
            if (descriptor.Writes(c)) {
                registers_->address.at(c) = ShaderTruncate(first.at(c));
            }
        }
        return;
    default:
        result_.unsupported = true;
        return;
    }
    Write(operands.destination, descriptor, result);
}

void Machine::MultiplyAdd(std::uint32_t word) {
    const shader::Operands operands = shader::MultiplyAddOperands(word);
    const Vec4 first = Operand(operands, 0);
    const Vec4 second = Operand(operands, 1);
    const Vec4 third = Operand(operands, 2);
    Vec4 result{};
    for (std::size_t c = 0; c < Components; ++c) {
        // Not fused: the product is rounded before the sum.
        result.at(c) = Round(ShaderMultiply(first.at(c), second.at(c))) + third.at(c);
    }
    Write(operands.destination, Descriptor{unit_->descriptors.at(operands.descriptor)}, result);
}

// Operators 6 and 7 always hold, as GBATEK observed; Azahar leaves the flag
// as it was.
void Machine::Compare(std::uint32_t word) {
    const shader::Operands operands = shader::ArithmeticOperands(word);
    const Vec4 first = Operand(operands, 0);
    const Vec4 second = Operand(operands, 1);
    const std::array<std::uint32_t, 2> operators = shader::CompareOperators(word);
    for (std::size_t c = 0; c < 2; ++c) {
        const float a = first.at(c);
        const float b = second.at(c);
        bool holds = true;
        switch (operators.at(c)) {
        case 0:
            holds = a == b;
            break;
        case 1:
            holds = a != b;
            break;
        case 2:
            holds = a < b;
            break;
        case 3:
            holds = a <= b;
            break;
        case 4:
            holds = a > b;
            break;
        case 5:
            holds = a >= b;
            break;
        default:
            break;
        }
        registers_->conditions.at(c) = holds;
    }
}

bool Machine::Condition(std::uint32_t word) const {
    const shader::Condition condition = shader::ConditionOf(word);
    const bool x = condition.x_reference == registers_->conditions.at(0);
    const bool y = condition.y_reference == registers_->conditions.at(1);
    switch (condition.combine) {
    case 0:
        return x || y;
    case 1:
        return x && y;
    case 2:
        return x;
    default:
        return y;
    }
}

bool Machine::Bool(std::uint32_t word) const {
    return ((unit_->bool_uniforms >> shader::BoolUniformOf(word)) & 1U) != 0;
}

void Machine::FlowControl(std::uint32_t word, std::uint32_t code) {
    const auto [target, count] = shader::RangeOf(word);
    const auto call = [&] {
        calls_.Push({.end_address = target + count, .return_address = counter_ + 1});
        Jump(target);
    };
    const auto branch = [&](bool condition) {
        if (condition) {
            ifs_.Push({.else_address = target, .end_address = target + count});
        } else {
            Jump(target);
        }
    };
    switch (code) {
    case opcode::Nop:
        break;
    case opcode::End:
        stopped_ = true;
        break;
    case opcode::Break:
        breaking_ = true;
        break;
    case opcode::Breakc:
        breaking_ = Condition(word);
        break;
    case opcode::Jmpc:
        if (Condition(word)) {
            Jump(target);
        }
        break;
    case opcode::Jmpu:
        // Bit 0 of the count inverts the test. Source: GBATEK, JMPU.
        if (Bool(word) == ((count & 1U) == 0)) {
            Jump(target);
        }
        break;
    case opcode::Call:
        call();
        break;
    case opcode::Callc:
        if (Condition(word)) {
            call();
        }
        break;
    case opcode::Callu:
        if (Bool(word)) {
            call();
        }
        break;
    case opcode::Ifc:
        branch(Condition(word));
        break;
    case opcode::Ifu:
        branch(Bool(word));
        break;
    case opcode::Loop: {
        // INT.x + 1 iterations, aL from INT.y stepping by INT.z. The integer
        // uniform is numbered in bits 22 and 23. The counter to restore is
        // the one the loop starts with, as Azahar records it.
        const std::uint32_t parameters = unit_->int_uniforms.at(shader::IntUniformOf(word));
        const auto start = static_cast<std::int32_t>(Field(parameters, 8, 8));
        registers_->address.at(2) = start;
        loops_.Push({.entry_address = counter_ + 1,
                     .end_address = target + 1,
                     .remaining = static_cast<std::uint8_t>(Field(parameters, 0, 8)),
                     .increment = static_cast<std::uint8_t>(Field(parameters, 16, 8)),
                     .previous_counter = start});
        break;
    }
    case opcode::Emit:
        if (run_->emitter == nullptr ||
            !run_->emitter->Emit(registers_->outputs.at(registers_->output_bank))) {
            result_.unsupported = true;
        }
        registers_->output_bank ^= 1U;
        break;
    case opcode::SetEmit:
        if (run_->emitter == nullptr) {
            result_.unsupported = true;
        } else {
            const shader::EmitSetup setup = shader::EmitSetupOf(word);
            run_->emitter->Configure(setup.vertex, setup.primitive, setup.reversed);
        }
        break;
    default:
        result_.unsupported = true;
        break;
    }
}

// After every instruction, each stack compares its innermost scope's end
// with the address after the instruction. Calls close first, up to four at
// once, then an if, then a loop, each overriding the counter the one before
// set, so that a jump on the last instruction of a scope is never taken.
// When four calls close at once the last does not move the counter, a
// hardware fault Azahar reproduces. Source: GBATEK, Control Flow; Azahar's
// interpreter for the fault and for a loop's restored counter.
void Machine::CloseScopes(std::uint32_t old_counter) {
    constexpr std::uint32_t MostCallsClosed = 4;
    std::uint32_t next = old_counter + 1;
    for (std::uint32_t closed = 0; closed < MostCallsClosed; ++closed) {
        if (calls_.Empty() || calls_.Top().end_address != next) {
            break;
        }
        if (closed < MostCallsClosed - 1) {
            counter_ = calls_.Top().return_address;
            next = counter_;
        }
        calls_.Pop();
    }
    if (!ifs_.Empty() && ifs_.Top().else_address == old_counter + 1) {
        counter_ = ifs_.Top().end_address;
        ifs_.Pop();
    }
    if (!loops_.Empty() && (loops_.Top().end_address == old_counter + 1 || breaking_)) {
        LoopScope& loop = loops_.Top();
        registers_->address.at(2) += loop.increment;
        if (!breaking_ && loop.remaining != 0) {
            --loop.remaining;
            counter_ = loop.entry_address;
        } else {
            counter_ = loop.end_address;
            // Only a loop inside another restores the counter.
            if (loops_.Size() > 1) {
                registers_->address.at(2) = loop.previous_counter;
            }
            loops_.Pop();
        }
    }
}

}  // namespace

bool GeometryEmitter::Emit(const ShaderRegisters::Bank& outputs) {
    if (vertex_ >= Vertices) {
        return false;
    }
    std::size_t packed = 0;
    for (std::size_t output = 0; output < ShaderRegisters::Count; ++output) {
        if (((output_mask_ >> output) & 1U) != 0) {
            buffer_.at(vertex_).at(packed) = outputs.at(output);
            ++packed;
        }
    }
    if (primitive_) {
        primitives_.push_back({.vertices = buffer_, .reversed = reversed_});
    }
    return true;
}

ShaderRunResult
RunShader(const ShaderUnitState& unit, ShaderRegisters& registers, const ShaderRun& run) {
    return Machine{unit, registers, run}.Run();
}

}  // namespace hexlith::domain::gpu
