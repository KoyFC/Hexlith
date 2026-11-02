#include "domain/lift/emit.h"

#include "domain/arm/operation.h"
#include "domain/arm/primitives/transfer.h"
#include "domain/lift/ir.h"
#include "domain/lift/lower.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace hexlith::domain::lift {
namespace {

namespace arm = domain::arm;
using ir::Operand;
using ir::OpKind;

constexpr std::array<std::string_view, 15> ConditionNames = {"Equal",
                                                             "NotEqual",
                                                             "CarrySet",
                                                             "CarryClear",
                                                             "Negative",
                                                             "NotNegative",
                                                             "Overflow",
                                                             "NoOverflow",
                                                             "Higher",
                                                             "LowerOrSame",
                                                             "GreaterOrEqual",
                                                             "Less",
                                                             "Greater",
                                                             "LessOrEqual",
                                                             "Always"};

// A 32-bit Thumb instruction keeps its second halfword above the first.
constexpr std::uint32_t ThumbHalfwordBits = 16;
constexpr std::uint32_t ThumbHalfwordMask = 0xFFFF;

constexpr std::array<std::string_view, 5> ShiftNames = {
    "LogicalLeft", "LogicalRight", "ArithmeticRight", "RotateRight", "RotateRightExtended"};

constexpr std::array<std::string_view, 5> WidthNames = {
    "Byte", "SignedByte", "Half", "SignedHalf", "Word"};

constexpr std::array<std::string_view, ir::FlagCount> FlagFields = {
    "negative", "zero", "carry", "overflow", "saturated"};

std::string OperationName(std::uint32_t index);
std::string Hex(std::uint32_t value);

std::string_view ConditionName(arm::Condition condition) {
    return condition == arm::Condition::Unconditional
               ? "Unconditional"
               : ConditionNames.at(static_cast<std::size_t>(condition));
}

constexpr std::array<std::string_view, 4> OperandFormNames = {
    "None", "Immediate", "Register", "ShiftedRegister"};

// The fields of a decoded instruction that differ from a default one, as the
// designated initializers of arm::Instruction, in the order it declares them.
std::string InstructionFields(const arm::Instruction& instruction) {
    const arm::Instruction defaults{};
    std::vector<std::string> fields;
    const auto number = [&fields](std::string_view name, auto value, auto fallback) {
        if (value != fallback) {
            fields.emplace_back(std::format(".{} = {}", name, static_cast<std::int64_t>(value)));
        }
    };
    const auto flag = [&fields](std::string_view name, bool value, bool fallback) {
        if (value != fallback) {
            fields.emplace_back(std::format(".{} = {}", name, value ? "true" : "false"));
        }
    };
    const auto enumerated =
        [&fields](std::string_view name, std::string_view type, auto value, auto fallback) {
            if (value != fallback) {
                fields.emplace_back(std::format(
                    ".{} = static_cast<arm::{}>({})", name, type, static_cast<unsigned>(value)));
            }
        };
    fields.emplace_back(std::format(".encoding = {}", Hex(instruction.encoding)));
    fields.emplace_back(
        std::format(".operation = arm::Operation::{}",
                    OperationName(static_cast<std::uint32_t>(instruction.operation))));
    if (instruction.condition != defaults.condition) {
        fields.emplace_back(
            std::format(".condition = arm::Condition::{}", ConditionName(instruction.condition)));
    }
    if (instruction.instruction_set != defaults.instruction_set) {
        fields.emplace_back(".instruction_set = arm::InstructionSet::Thumb");
    }
    number("width", instruction.width, defaults.width);
    number("destination", instruction.destination, defaults.destination);
    number("destination_high", instruction.destination_high, defaults.destination_high);
    number("first", instruction.first, defaults.first);
    number("second", instruction.second, defaults.second);
    number("third", instruction.third, defaults.third);
    if (instruction.operand != defaults.operand) {
        fields.emplace_back(
            std::format(".operand = arm::OperandForm::{}",
                        OperandFormNames.at(static_cast<std::size_t>(instruction.operand))));
    }
    number("immediate", instruction.immediate, defaults.immediate);
    if (instruction.shift != defaults.shift) {
        fields.emplace_back(
            std::format(".shift = arm::ShiftKind::{}",
                        ShiftNames.at(static_cast<std::size_t>(instruction.shift))));
    }
    number("shift_amount", instruction.shift_amount, defaults.shift_amount);
    number("shift_register", instruction.shift_register, defaults.shift_register);
    number("registers", instruction.registers, defaults.registers);
    number("branch_offset", instruction.branch_offset, defaults.branch_offset);
    flag("updates_flags", instruction.updates_flags, defaults.updates_flags);
    flag("preindexed", instruction.preindexed, defaults.preindexed);
    flag("add_offset", instruction.add_offset, defaults.add_offset);
    flag("writes_back", instruction.writes_back, defaults.writes_back);
    flag("saved_status", instruction.saved_status, defaults.saved_status);
    flag("left_half_high", instruction.left_half_high, defaults.left_half_high);
    flag("right_half_high", instruction.right_half_high, defaults.right_half_high);
    number("coprocessor", instruction.coprocessor, defaults.coprocessor);
    number("coprocessor_opcode1", instruction.coprocessor_opcode1, defaults.coprocessor_opcode1);
    number("coprocessor_opcode2", instruction.coprocessor_opcode2, defaults.coprocessor_opcode2);
    enumerated(
        "parallel_prefix", "ParallelPrefix", instruction.parallel_prefix, defaults.parallel_prefix);
    enumerated(
        "parallel_shape", "ParallelShape", instruction.parallel_shape, defaults.parallel_shape);
    number("vfp_destination", instruction.vfp_destination, defaults.vfp_destination);
    number("vfp_first", instruction.vfp_first, defaults.vfp_first);
    number("vfp_second", instruction.vfp_second, defaults.vfp_second);
    flag("vfp_destination_double",
         instruction.vfp_destination_double,
         defaults.vfp_destination_double);
    flag("vfp_sources_double", instruction.vfp_sources_double, defaults.vfp_sources_double);
    number("vfp_count", instruction.vfp_count, defaults.vfp_count);
    flag("integer_signed", instruction.integer_signed, defaults.integer_signed);
    flag("rounds_towards_zero", instruction.rounds_towards_zero, defaults.rounds_towards_zero);
    std::string text;
    for (const std::string& field : fields) {
        text.append(text.empty() ? "" : ", ").append(field);
    }
    return text;
}

std::string OperationName(std::uint32_t index) {
    using arm::Operation;
    switch (static_cast<Operation>(index)) {
    case Operation::And:
        return "And";
    case Operation::Eor:
        return "Eor";
    case Operation::Sub:
        return "Sub";
    case Operation::Rsb:
        return "Rsb";
    case Operation::Add:
        return "Add";
    case Operation::Adc:
        return "Adc";
    case Operation::Sbc:
        return "Sbc";
    case Operation::Rsc:
        return "Rsc";
    case Operation::Tst:
        return "Tst";
    case Operation::Teq:
        return "Teq";
    case Operation::Cmp:
        return "Cmp";
    case Operation::Cmn:
        return "Cmn";
    case Operation::Orr:
        return "Orr";
    case Operation::Mov:
        return "Mov";
    case Operation::Bic:
        return "Bic";
    case Operation::Mvn:
        return "Mvn";
    case Operation::Sxtab:
        return "Sxtab";
    case Operation::Sxtah:
        return "Sxtah";
    case Operation::Sxtb:
        return "Sxtb";
    case Operation::Sxth:
        return "Sxth";
    case Operation::Uxtab:
        return "Uxtab";
    case Operation::Uxtah:
        return "Uxtah";
    case Operation::Uxtb:
        return "Uxtb";
    case Operation::Uxth:
        return "Uxth";
    case Operation::LoadExclusive:
        return "LoadExclusive";
    case Operation::StoreExclusive:
        return "StoreExclusive";
    case Operation::LoadExclusiveByte:
        return "LoadExclusiveByte";
    case Operation::StoreExclusiveByte:
        return "StoreExclusiveByte";
    case Operation::LoadExclusiveHalf:
        return "LoadExclusiveHalf";
    case Operation::StoreExclusiveHalf:
        return "StoreExclusiveHalf";
    case Operation::LoadExclusiveDouble:
        return "LoadExclusiveDouble";
    case Operation::StoreExclusiveDouble:
        return "StoreExclusiveDouble";
    case Operation::ClearExclusive:
        return "ClearExclusive";
    case Operation::LoadDouble:
        return "LoadDouble";
    case Operation::StoreDouble:
        return "StoreDouble";
    case Operation::Umull:
        return "Umull";
    case Operation::Umlal:
        return "Umlal";
    case Operation::Umaal:
        return "Umaal";
    case Operation::Smull:
        return "Smull";
    case Operation::Smlal:
        return "Smlal";
    case Operation::SmulHalf:
        return "SmulHalf";
    case Operation::SmlaHalf:
        return "SmlaHalf";
    case Operation::SmlalHalf:
        return "SmlalHalf";
    case Operation::SmulwHalf:
        return "SmulwHalf";
    case Operation::SmlawHalf:
        return "SmlawHalf";
    case Operation::CountLeadingZeros:
        return "CountLeadingZeros";
    case Operation::VfpMultiplyAccumulate:
        return "VfpMultiplyAccumulate";
    case Operation::VfpMultiplySubtract:
        return "VfpMultiplySubtract";
    case Operation::VfpNegatedMultiplyAccumulate:
        return "VfpNegatedMultiplyAccumulate";
    case Operation::VfpNegatedMultiplySubtract:
        return "VfpNegatedMultiplySubtract";
    case Operation::VfpMultiply:
        return "VfpMultiply";
    case Operation::VfpNegatedMultiply:
        return "VfpNegatedMultiply";
    case Operation::VfpAdd:
        return "VfpAdd";
    case Operation::VfpSubtract:
        return "VfpSubtract";
    case Operation::VfpDivide:
        return "VfpDivide";
    case Operation::VfpCopy:
        return "VfpCopy";
    case Operation::VfpAbsolute:
        return "VfpAbsolute";
    case Operation::VfpNegate:
        return "VfpNegate";
    case Operation::VfpSquareRoot:
        return "VfpSquareRoot";
    case Operation::VfpCompare:
        return "VfpCompare";
    case Operation::VfpCompareSignalling:
        return "VfpCompareSignalling";
    case Operation::VfpConvertPrecision:
        return "VfpConvertPrecision";
    case Operation::VfpConvertFromInteger:
        return "VfpConvertFromInteger";
    case Operation::VfpConvertToInteger:
        return "VfpConvertToInteger";
    case Operation::VfpMoveToSingle:
        return "VfpMoveToSingle";
    case Operation::VfpMoveFromSingle:
        return "VfpMoveFromSingle";
    case Operation::VfpMoveToHalf:
        return "VfpMoveToHalf";
    case Operation::VfpMoveFromHalf:
        return "VfpMoveFromHalf";
    case Operation::VfpMoveToPair:
        return "VfpMoveToPair";
    case Operation::VfpMoveFromPair:
        return "VfpMoveFromPair";
    case Operation::VfpMoveToSystem:
        return "VfpMoveToSystem";
    case Operation::VfpMoveFromSystem:
        return "VfpMoveFromSystem";
    case Operation::VfpLoad:
        return "VfpLoad";
    case Operation::VfpStore:
        return "VfpStore";
    case Operation::VfpLoadMultiple:
        return "VfpLoadMultiple";
    case Operation::VfpStoreMultiple:
        return "VfpStoreMultiple";
    default:
        break;
    }
    return std::format("static_cast<Operation>({})", index);
}

std::string Hex(std::uint32_t value) {
    return std::format("0x{:08X}U", value);
}

std::string Label(std::uint32_t address) {
    return std::format("bb_{:08X}", address);
}

// Writes the C++ of one function. Temporaries are named by the expression that
// produces them, which for a result of a primitive is a field of a local.
class Emitter {
public:
    Emitter(const Function& function, std::string_view name, const EmitContext& context)
        : function_(&function), name_(name), context_(&context) {}

    EmittedFunction Run() {
        EmittedFunction emitted;
        emitted.name = name_;
        out_ +=
            std::format("// {:08X}, {}, {} block(s)\n",
                        function_->entry,
                        function_->instruction_set == arm::InstructionSet::Arm ? "ARM" : "Thumb",
                        function_->blocks.size());
        out_ += std::format("runtime::Pc {}(runtime::Context& c, runtime::Pc pc) {{\n", name_);
        out_ += "    [[maybe_unused]] arm::CpuState& cpu = *c.cpu;\n";
        out_ += "    [[maybe_unused]] memory::ProcessMemory& mem = *c.memory;\n";
        EntrySwitch(emitted);
        for (const BasicBlock& block : function_->blocks) {
            Block(block);
        }
        out_ += "}\n";
        emitted.text = std::move(out_);
        return emitted;
    }

private:
    void Line(std::string_view text) {
        out_ += std::format("{}{}\n", std::string(indent_ * 4, ' '), text);
    }

    // The function can be entered at the start of any of its blocks, which is
    // what lets the engine pick up where the guest left off.
    void EntrySwitch(EmittedFunction& emitted) {
        indent_ = 1;
        Line("switch (pc) {");
        for (const BasicBlock& block : function_->blocks) {
            Line(std::format("case {}: goto {};", Hex(block.start), Label(block.start)));
            emitted.entries.push_back(block.start);
        }
        Line("default: return pc;");
        Line("}");
    }

    void Block(const BasicBlock& block) {
        indent_ = 0;
        Line(std::format("{}:", Label(block.start)));
        indent_ = 1;
        const std::size_t cost = block.Cost();
        if (cost != 0) {
            Line(std::format(
                "if (c.budget < {}) return runtime::OutOfBudget(c, {});", cost, Hex(block.start)));
            Line(std::format("c.budget -= {};", cost));
        }
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            Instruction(block.instructions.at(index), cost - index);
        }
        EndOfBlock(block);
    }

    // `unexecuted` is how many of the block's instructions have not run when this
    // one starts, itself included: what a bail gives back to the clock.
    void Instruction(const PlacedInstruction& placed, std::size_t unexecuted) {
        const ir::Instruction lowered = Lower(placed);
        if (lowered.ops.empty()) {
            // A hint, or a branch whose effect is the block's exit.
            Line(std::format(
                "// {:08X} {}", placed.address, arm::Mnemonic(placed.instruction.operation)));
            return;
        }
        Line(std::format(
            "// {:08X} {}", placed.address, arm::Mnemonic(placed.instruction.operation)));
        Line("{");
        ++indent_;

        const bool guarded = lowered.condition != arm::Condition::Always;
        if (guarded) {
            Line(std::format("if (runtime::Passes(cpu, arm::Condition::{})) {{",
                             ConditionNames.at(static_cast<std::size_t>(lowered.condition))));
            ++indent_;
        }
        values_.clear();
        placed_ = &placed;
        for (const ir::Op& op : lowered.ops) {
            Operation(op, placed.address, unexecuted);
        }
        if (guarded) {
            --indent_;
            Line("}");
        }
        --indent_;
        Line("}");
    }

    [[nodiscard]] std::string Value(const Operand& operand) const {
        switch (operand.kind) {
        case Operand::Kind::Immediate:
            if (operand.type == ir::Type::Bit) {
                return operand.value != 0 ? "true" : "false";
            }
            return Hex(operand.value);
        case Operand::Kind::Temporary:
            return values_.at(operand.value);
        case Operand::Kind::None:
            break;
        }
        return "0U";
    }

    // Gives a temporary the name a later operation will read it by.
    std::string Define(std::uint32_t id, std::string expression) {
        values_[id] = std::move(expression);
        return values_[id];
    }

    void Const(std::uint32_t id, std::string_view type, const std::string& expression) {
        const std::string name = std::format("t{}", id);
        Line(std::format("const {} {} = {};", type, name, expression));
        Define(id, name);
    }

    void Operation(const ir::Op& op, std::uint32_t address, std::size_t unexecuted) {
        const std::string a = Value(op.a);
        const std::string b = Value(op.b);
        switch (op.kind) {
        case OpKind::ReadRegister:
            Const(op.destination, "std::uint32_t", std::format("cpu.registers[{}]", op.index));
            break;
        case OpKind::WriteRegister:
            Line(std::format("cpu.registers[{}] = {};", op.index, a));
            break;
        case OpKind::ReadFlag:
            Const(op.destination, "bool", std::format("cpu.{}", FlagFields.at(op.index)));
            break;
        case OpKind::WriteFlag:
            Line(std::format("cpu.{} = {};", FlagFields.at(op.index), a));
            break;
        case OpKind::WriteThumb:
            Line(std::format("cpu.thumb = {};", a));
            break;
        case OpKind::ReadThreadId:
            Const(op.destination,
                  "std::uint32_t",
                  static_cast<ir::ThreadId>(op.index) == ir::ThreadId::ReadWrite
                      ? "cpu.thread_id_read_write"
                      : "cpu.thread_id_read_only");
            break;
        case OpKind::WriteThreadId:
            Line(std::format("cpu.thread_id_read_write = {};", a));
            break;
        case OpKind::Add:
            Const(op.destination, "std::uint32_t", std::format("{} + {}", a, b));
            break;
        case OpKind::Subtract:
            Const(op.destination, "std::uint32_t", std::format("{} - {}", a, b));
            break;
        case OpKind::And:
            Const(op.destination, "std::uint32_t", std::format("{} & {}", a, b));
            break;
        case OpKind::Or:
            Const(op.destination, "std::uint32_t", std::format("{} | {}", a, b));
            break;
        case OpKind::Xor:
            Const(op.destination, "std::uint32_t", std::format("{} ^ {}", a, b));
            break;
        case OpKind::Multiply:
            Const(op.destination, "std::uint32_t", std::format("{} * {}", a, b));
            break;
        case OpKind::Equal:
            Const(op.destination, "bool", std::format("{} == {}", a, b));
            break;
        case OpKind::NotEqual:
            Const(op.destination, "bool", std::format("{} != {}", a, b));
            break;
        case OpKind::Negative:
            Const(op.destination, "bool", std::format("({} >> 31U) != 0", a));
            break;
        case OpKind::Select:
            Const(op.destination, "std::uint32_t", std::format("{} ? {} : {}", a, b, Value(op.c)));
            break;
        case OpKind::Extend:
            Const(op.destination,
                  "std::uint32_t",
                  std::format("primitives::Extend(arm::Operation::{}, {}, {})",
                              OperationName(op.index),
                              a,
                              b));
            break;
        case OpKind::Shift:
            Shift(op, a, b);
            break;
        case OpKind::Alu:
            Alu(op, a, b);
            break;
        case OpKind::ConditionPassed:
            Const(op.destination,
                  "bool",
                  std::format("primitives::ConditionPassed(arm::Condition::{}, {{{}, {}, {}, {}}})",
                              ConditionNames.at(op.index),
                              a,
                              b,
                              Value(op.c),
                              Value(op.d)));
            break;
        case OpKind::Load:
            Load(op, a, address, unexecuted);
            break;
        case OpKind::Store:
            Line(std::format("if (!primitives::StoreValue(mem, {}, {}, primitives::Width::{})) "
                             "return runtime::Bail(c, {}, {});",
                             a,
                             b,
                             WidthNames.at(op.index),
                             Hex(address),
                             unexecuted));
            break;
        case OpKind::BailIf:
            Line(std::format(
                "if ({}) return runtime::Bail(c, {}, {});", a, Hex(address), unexecuted));
            break;
        case OpKind::Interpret:
            Interpret(address, unexecuted);
            break;
        case OpKind::Execute:
            Execute(address, unexecuted);
            break;
        }
    }

    // The interpreter's handler does the work, with the instruction a constant
    // written out field by field, so that the compiler resolves which handler
    // and sees the registers it names.
    void Execute(std::uint32_t address, std::size_t unexecuted) {
        Line(std::format("static constexpr arm::Instruction decoded{{{}}};",
                         InstructionFields(placed_->instruction)));
        Line(std::format("if (const arm::Stop stop = runtime::ExecuteHandled(c, {}, decoded); "
                         "stop.Stopped()) return runtime::StoppedBy(c, stop, {});",
                         Hex(address),
                         unexecuted - 1));
    }

    // The interpreter runs what the lifter does not translate, and decoding is
    // most of what that costs, so the instruction is decoded once, the first time
    // the site runs, and kept.
    void Interpret(std::uint32_t address, std::size_t unexecuted) {
        const arm::Instruction& instruction = placed_->instruction;
        const std::string decode =
            instruction.instruction_set == arm::InstructionSet::Arm
                ? std::format("arm::DecodeArm({})", Hex(instruction.encoding))
                : std::format("arm::DecodeThumb({}, {})",
                              Hex(instruction.encoding & ThumbHalfwordMask),
                              Hex(instruction.encoding >> ThumbHalfwordBits));
        Line(std::format("static const arm::Instruction decoded = {};", decode));
        Line(std::format("if (const arm::Stop stop = runtime::ExecuteDecoded(c, {}, decoded); "
                         "stop.Stopped()) return runtime::StoppedBy(c, stop, {});",
                         Hex(address),
                         unexecuted - 1));
    }

    void Shift(const ir::Op& op, const std::string& a, const std::string& b) {
        const std::string local = std::format("shift{}", op.destination);
        Line(std::format("const primitives::ShiftResult {} = primitives::Shift("
                         "arm::ShiftKind::{}, {}, {}, {});",
                         local,
                         ShiftNames.at(op.index),
                         a,
                         b,
                         Value(op.c)));
        Define(op.destination, local + ".value");
        if (op.destination2 != ir::NoTemporary) {
            Define(op.destination2, local + ".carry");
        }
    }

    void Alu(const ir::Op& op, const std::string& a, const std::string& b) {
        const std::string local = std::format("alu{}", op.destination);
        Line(std::format("const primitives::AluResult {} = primitives::ComputeAlu("
                         "arm::Operation::{}, {}, {{{}, {}}}, {});",
                         local,
                         OperationName(op.index),
                         a,
                         b,
                         Value(op.c),
                         Value(op.d)));
        Define(op.destination, local + ".value");
        Define(op.destination2, local + ".carry");
        Define(op.destination3, local + ".overflow");
    }

    void Load(const ir::Op& op,
              const std::string& address_value,
              std::uint32_t address,
              std::size_t unexecuted) {
        const std::string local = std::format("load{}", op.destination);
        Line(std::format("const std::optional<std::uint32_t> {} = "
                         "primitives::LoadValue(mem, {}, primitives::Width::{});",
                         local,
                         address_value,
                         WidthNames.at(op.index)));
        Line(std::format(
            "if (!{}) return runtime::Bail(c, {}, {});", local, Hex(address), unexecuted));
        Define(op.destination, std::format("(*{})", local));
    }

    // --- Exits ------------------------------------------------------------------

    [[nodiscard]] static std::string Condition(const Exit& exit) {
        return std::format("runtime::Passes(cpu, arm::Condition::{})",
                           ConditionNames.at(static_cast<std::size_t>(exit.condition)));
    }

    // Runs `body` for the exit, only when its condition passes.
    void When(const Exit& exit, const std::function<void()>& body) {
        if (!arm::IsConditional(exit.condition)) {
            body();
            return;
        }
        Line(std::format("if ({}) {{", Condition(exit)));
        ++indent_;
        body();
        --indent_;
        Line("}");
    }

    void GoToNext(const Exit& exit) {
        if (exit.HasNext()) {
            Line(std::format("goto {};", Label(exit.next)));
        }
    }

    // How a call or a tail call reaches its target: a call of the lifted function
    // there, or just its address when there is none.
    [[nodiscard]] std::string Reach(const Exit& exit) const {
        const std::optional<std::string> name =
            context_->function_at ? context_->function_at(exit.target, exit.target_set)
                                  : std::nullopt;
        return name.has_value() ? std::format("{}(c, {})", *name, Hex(exit.target))
                                : Hex(exit.target);
    }

    void SetInstructionSetIfItChanges(const Exit& exit) {
        if (exit.target_set != function_->instruction_set) {
            Line(std::format("cpu.thumb = {};",
                             exit.target_set == arm::InstructionSet::Thumb ? "true" : "false"));
        }
    }

    void EndOfBlock(const BasicBlock& block) {
        const Exit& exit = block.exit;
        switch (exit.kind) {
        case ExitKind::Fallthrough:
            Line(std::format("goto {};", Label(exit.next)));
            break;
        case ExitKind::Jump:
            When(exit, [&] { Line(std::format("goto {};", Label(exit.target))); });
            GoToNext(exit);
            break;
        case ExitKind::TailCall:
            When(exit, [&] {
                SetInstructionSetIfItChanges(exit);
                Line(std::format("return {};", Reach(exit)));
            });
            GoToNext(exit);
            break;
        case ExitKind::Call:
            When(exit, [&] {
                SetInstructionSetIfItChanges(exit);
                CallAndReturn(exit, Reach(exit));
            });
            GoToNext(exit);
            break;
        case ExitKind::IndirectCall:
            When(exit, [&] { CallAndReturn(exit, "runtime::Dispatch(c, cpu.registers[15])"); });
            GoToNext(exit);
            break;
        case ExitKind::IndirectJump:
            When(exit, [&] { Line("return runtime::Dispatch(c, cpu.registers[15]);"); });
            GoToNext(exit);
            break;
        case ExitKind::Return:
            When(exit, [&] { Line("return cpu.registers[15];"); });
            GoToNext(exit);
            break;
        case ExitKind::Switch:
            SwitchExit(exit);
            break;
        case ExitKind::SystemCall:
            GoToNext(exit);
            break;
        case ExitKind::Trap:
            Line(std::format("return runtime::OutOfBudget(c, {});", Hex(exit.target)));
            break;
        }
    }

    // A call comes back when the callee answers the address after it, and
    // anything else is passed up.
    void CallAndReturn(const Exit& exit, const std::string& call) {
        // In braces so that the label of the next block can be reached from the
        // entry switch without jumping over the initialisation.
        Line("{");
        ++indent_;
        Line(std::format("const runtime::Pc back = {};", call));
        Line(std::format("if (back != {}) return back;", Hex(exit.next)));
        --indent_;
        Line("}");
        Line(std::format("goto {};", Label(exit.next)));
    }

    // The cases are blocks of this function, and jumping to them is a goto. An
    // index outside the table, or an address the analysis did not give a block,
    // leaves the way an indirect jump does.
    void SwitchExit(const Exit& exit) {
        When(exit, [&] {
            Line("switch (cpu.registers[15]) {");
            std::vector<std::uint32_t> seen;
            for (const std::uint32_t target : exit.cases) {
                if (function_->BlockAt(target) != nullptr &&
                    std::ranges::find(seen, target) == seen.end()) {
                    seen.push_back(target);
                    Line(std::format("case {}: goto {};", Hex(target), Label(target)));
                }
            }
            Line("default: return runtime::Dispatch(c, cpu.registers[15]);");
            Line("}");
        });
        GoToNext(exit);
    }

    const Function* function_;
    std::string name_;
    const EmitContext* context_;
    std::string out_;
    std::size_t indent_ = 0;
    std::map<std::uint32_t, std::string> values_;
    const PlacedInstruction* placed_ = nullptr;
};

}  // namespace

std::string DefaultFunctionName(std::uint32_t entry, arm::InstructionSet instruction_set) {
    return std::format(
        "fn_{:08X}{}", entry, instruction_set == arm::InstructionSet::Thumb ? "_t" : "");
}

std::string DeclarationOf(std::string_view name) {
    return std::format("runtime::Pc {}(runtime::Context& c, runtime::Pc pc);", name);
}

EmittedFunction
EmitFunction(const Function& function, std::string_view name, const EmitContext& context) {
    return Emitter{function, name, context}.Run();
}

}  // namespace hexlith::domain::lift
