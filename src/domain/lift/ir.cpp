#include "domain/lift/ir.h"

#include "domain/arm/condition.h"

#include <format>
#include <string_view>

namespace hexlith::domain::lift::ir {
namespace {

std::string_view Name(OpKind kind) {
    switch (kind) {
    case OpKind::ReadRegister:
        return "read-register";
    case OpKind::WriteRegister:
        return "write-register";
    case OpKind::ReadFlag:
        return "read-flag";
    case OpKind::WriteFlag:
        return "write-flag";
    case OpKind::WriteThumb:
        return "write-thumb";
    case OpKind::ReadThreadId:
        return "read-thread-id";
    case OpKind::WriteThreadId:
        return "write-thread-id";
    case OpKind::Add:
        return "add";
    case OpKind::Subtract:
        return "subtract";
    case OpKind::And:
        return "and";
    case OpKind::Or:
        return "or";
    case OpKind::Xor:
        return "xor";
    case OpKind::Multiply:
        return "multiply";
    case OpKind::Equal:
        return "equal";
    case OpKind::NotEqual:
        return "not-equal";
    case OpKind::Negative:
        return "negative";
    case OpKind::Select:
        return "select";
    case OpKind::Shift:
        return "shift";
    case OpKind::Alu:
        return "alu";
    case OpKind::Extend:
        return "extend";
    case OpKind::ConditionPassed:
        return "condition-passed";
    case OpKind::Load:
        return "load";
    case OpKind::Store:
        return "store";
    case OpKind::BailIf:
        return "bail-if";
    case OpKind::Interpret:
        return "interpret";
    case OpKind::Execute:
        return "execute";
    }
    return "unknown";
}

std::string Describe(const Operand& operand) {
    switch (operand.kind) {
    case Operand::Kind::None:
        return "";
    case Operand::Kind::Immediate:
        if (operand.type == Type::Bit) {
            return operand.value != 0 ? "true" : "false";
        }
        return std::format("{:#x}", operand.value);
    case Operand::Kind::Temporary:
        return std::format("t{}", operand.value);
    }
    return "";
}

std::string Destinations(const Op& op) {
    std::string text;
    for (const std::uint32_t id : {op.destination, op.destination2, op.destination3}) {
        if (id != NoTemporary) {
            text += std::format("{}t{}", text.empty() ? "" : ", ", id);
        }
    }
    return text;
}

}  // namespace

std::string Describe(const Op& op) {
    std::string text = Destinations(op);
    if (!text.empty()) {
        text += " = ";
    }
    text += Name(op.kind);
    text += std::format(" #{}", op.index);
    for (const Operand& operand : {op.a, op.b, op.c, op.d}) {
        if (operand.IsPresent()) {
            text += ' ';
            text += Describe(operand);
        }
    }
    return text;
}

std::string Describe(const Instruction& instruction) {
    const std::string_view suffix = arm::ConditionSuffix(instruction.condition);
    std::string text =
        std::format("{:08X} if {}\n", instruction.address, suffix.empty() ? "al" : suffix);
    for (const Op& op : instruction.ops) {
        text += std::format("  {}\n", Describe(op));
    }
    return text;
}

}  // namespace hexlith::domain::lift::ir
