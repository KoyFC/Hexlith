#ifndef HEXLITH_APPLICATION_LIFT_SURVEY_H
#define HEXLITH_APPLICATION_LIFT_SURVEY_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/lift/control_flow.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::application {

// What the control flow of the whole main executable looks like once every
// function in the analysis tables has been cut into blocks.
//
// This is the first question the lifter can answer about the real image, and it
// says what the lifted code will have to handle before any of it is written:
// how many calls go through a register, how many blocks end in a trap, how big
// the largest function is. The numbers only describe; nothing is written.

struct LiftSurveyRequest {
    // The decompressed .code image and the address its first byte is at.
    core::ByteSpan code;
    std::uint32_t base = 0;

    // functions.txt and regions.txt, as hexlith-analyze wrote them.
    std::string_view functions;
    std::string_view regions;
};

constexpr std::size_t ExitKindCount = 10;

struct LiftSurvey {
    std::size_t functions = 0;
    std::size_t arm_functions = 0;
    std::size_t thumb_functions = 0;
    std::size_t blocks = 0;
    std::size_t instructions = 0;

    // The largest function and the largest block, in instructions.
    std::size_t largest_function = 0;
    std::size_t largest_block = 0;

    // How many blocks end each way, indexed by domain::lift::ExitKind.
    std::array<std::size_t, ExitKindCount> exits{};

    // The blocks that end in an indirect jump, by the instruction that does it:
    // BX through a register, a load into the program counter, a block load
    // that is not a return, and arithmetic or a move.
    std::size_t indirect_through_register = 0;
    std::size_t indirect_through_load = 0;
    std::size_t indirect_through_block_load = 0;
    std::size_t indirect_through_arithmetic = 0;

    // Cases across every switch.
    std::size_t switch_cases = 0;

    // Where the traps are, in function order, so that each one can be looked at.
    std::vector<std::uint32_t> trap_addresses;

    // Functions with a block that ends in a trap, and functions that stopped at
    // the size limit.
    std::size_t functions_with_a_trap = 0;
    std::size_t functions_at_the_limit = 0;

    // Functions whose entry could not be decoded at all.
    std::size_t functions_without_code = 0;

    [[nodiscard]] std::size_t Exits(domain::lift::ExitKind kind) const {
        return exits.at(static_cast<std::size_t>(kind));
    }
};

core::Result<LiftSurvey> SurveyControlFlow(const LiftSurveyRequest& request);

std::string RenderLiftSurvey(const LiftSurvey& survey);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_LIFT_SURVEY_H
