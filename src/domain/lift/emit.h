#ifndef HEXLITH_DOMAIN_LIFT_EMIT_H
#define HEXLITH_DOMAIN_LIFT_EMIT_H

#include "domain/arm/instruction_set.h"
#include "domain/lift/control_flow.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::lift {

// What the emitter needs to know about the functions around the one it writes.
struct EmitContext {
    // The name of the lifted function that starts at an address in an
    // instruction set, if there is one. A call or a tail call to an address
    // without one leaves the lifted code for the engine to continue.
    std::function<std::optional<std::string>(std::uint32_t, arm::InstructionSet)> function_at;

    // Whether the word at an aligned address is one a relocation writes.
    // Nothing is assumed when it is empty.
    std::function<bool(std::uint32_t)> relocated;

    // Whether the code is a module's. Its addresses are then offsets from the
    // module's start, and every address the code computes or hands back is the
    // offset added to runtime::Context::base. The main executable's addresses
    // are where it is mapped, always.
    bool module = false;
};

struct EmittedFunction {
    std::string name;

    // The C++ definition.
    std::string text;

    // Every address the function can be entered at: where each block starts.
    std::vector<std::uint32_t> entries;
};

// The name a function at an address gets when nothing better is known.
std::string DefaultFunctionName(std::uint32_t entry, arm::InstructionSet instruction_set);

// The declaration every function shares, ending in a semicolon, for a header
// that lets functions call each other in any order.
std::string DeclarationOf(std::string_view name);

// Writes one function as C++ that includes domain/lift/runtime.h.
//
// The code is meant to be read. Each block is a label, each guest instruction a
// braced group commented with its address and mnemonic, and what the group does
// is a call to the primitive the interpreter calls. A block is charged to the
// clock when it is entered, and everything the lifted code cannot do leaves for
// the interpreter through the runtime.
[[nodiscard]] EmittedFunction
EmitFunction(const Function& function, std::string_view name, const EmitContext& context);

}  // namespace hexlith::domain::lift

#endif  // HEXLITH_DOMAIN_LIFT_EMIT_H
