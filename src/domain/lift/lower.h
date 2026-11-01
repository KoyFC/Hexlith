#ifndef HEXLITH_DOMAIN_LIFT_LOWER_H
#define HEXLITH_DOMAIN_LIFT_LOWER_H

#include "domain/lift/control_flow.h"
#include "domain/lift/ir.h"

namespace hexlith::domain::lift {

// Translates one decoded instruction into IR operations.
//
// This is the one piece of the lifter that restates what an instruction means,
// and the interpreter is what it has to agree with. It mirrors the interpreter's
// executors step for step, down to the order of reads and writes, and calls the
// same shared rules for what is unpredictable. An instruction it does not
// translate becomes a single Interpret operation, which is exact by
// construction, so the coverage of this function can grow one class of
// instruction at a time without anything depending on the rest.
//
// A condition that cannot be known until the block runs, such as an address that
// is not aligned or a load the memory refuses, becomes a bail: the lifted code
// leaves for the interpreter at this instruction with the state as it was, and
// the interpreter does what it does. Everything that can be settled by looking
// at the instruction, such as registers the architecture calls unpredictable, is
// settled here and sends the whole instruction to the interpreter.
[[nodiscard]] ir::Instruction Lower(const PlacedInstruction& placed);

}  // namespace hexlith::domain::lift

#endif  // HEXLITH_DOMAIN_LIFT_LOWER_H
