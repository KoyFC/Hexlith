#include "application/lift_survey.h"

#include "domain/analysis/tables.h"

#include <algorithm>
#include <array>
#include <format>
#include <unordered_set>
#include <vector>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;
namespace lift = domain::lift;

constexpr std::array<std::string_view, ExitKindCount> ExitNames = {"fallthrough",
                                                                   "jump",
                                                                   "tail call",
                                                                   "call",
                                                                   "indirect call",
                                                                   "indirect jump",
                                                                   "return",
                                                                   "switch",
                                                                   "system call",
                                                                   "trap"};

void CountIndirect(LiftSurvey& survey, domain::arm::Operation operation) {
    using Operation = domain::arm::Operation;
    switch (operation) {
    case Operation::BranchExchange:
        ++survey.indirect_through_register;
        break;
    case Operation::LoadWord:
        ++survey.indirect_through_load;
        break;
    case Operation::LoadMultiple:
        ++survey.indirect_through_block_load;
        break;
    default:
        ++survey.indirect_through_arithmetic;
        break;
    }
}

void Add(LiftSurvey& survey, const lift::Function& function) {
    ++survey.functions;
    if (function.instruction_set == domain::arm::InstructionSet::Arm) {
        ++survey.arm_functions;
    } else {
        ++survey.thumb_functions;
    }

    survey.blocks += function.blocks.size();
    survey.instructions += function.InstructionCount();
    survey.largest_function = std::max(survey.largest_function, function.InstructionCount());

    bool trapped = false;
    for (const lift::BasicBlock& block : function.blocks) {
        survey.largest_block = std::max(survey.largest_block, block.Cost());
        ++survey.exits.at(static_cast<std::size_t>(block.exit.kind));
        survey.switch_cases += block.exit.cases.size();
        trapped = trapped || block.exit.kind == lift::ExitKind::Trap;
        if (block.exit.kind == lift::ExitKind::Trap) {
            survey.trap_addresses.push_back(block.exit.target);
        }
        if (block.exit.kind == lift::ExitKind::IndirectJump) {
            CountIndirect(survey, block.instructions.back().instruction.operation);
        }
    }
    if (trapped) {
        ++survey.functions_with_a_trap;
    }
    if (function.InstructionCount() >= lift::MaximumInstructions) {
        ++survey.functions_at_the_limit;
    }
    if (function.blocks.empty() || function.blocks.front().Cost() == 0) {
        ++survey.functions_without_code;
    }
}

}  // namespace

core::Result<LiftSurvey> SurveyControlFlow(const LiftSurveyRequest& request) {
    core::Result<std::vector<analysis::FunctionRange>> functions =
        analysis::ParseFunctions(request.functions);
    if (!functions) {
        return functions.Failure().WithContext("functions.txt");
    }
    core::Result<std::vector<analysis::Region>> regions = analysis::ParseRegions(request.regions);
    if (!regions) {
        return regions.Failure().WithContext("regions.txt");
    }
    std::ranges::sort(
        regions.Value(), {}, [](const analysis::Region& region) { return region.address; });

    std::unordered_set<std::uint32_t> entries;
    for (const analysis::FunctionRange& function : functions.Value()) {
        entries.insert(function.address);
    }

    const lift::CodeImage image{.bytes = request.code, .base = request.base};
    const lift::FlowContext context{.entries = &entries, .regions = regions.Value()};

    LiftSurvey survey;
    for (const analysis::FunctionRange& function : functions.Value()) {
        Add(survey,
            lift::BuildFunction(image, function.address, function.instruction_set, context));
    }
    return survey;
}

std::string RenderLiftSurvey(const LiftSurvey& survey) {
    std::string text;
    text.append(std::format("{} function(s): {} ARM, {} Thumb\n",
                            survey.functions,
                            survey.arm_functions,
                            survey.thumb_functions));
    text.append(
        std::format("{} block(s), {} instruction(s)\n", survey.blocks, survey.instructions));
    text.append(std::format("largest function {} instruction(s), largest block {}\n",
                            survey.largest_function,
                            survey.largest_block));
    for (std::size_t kind = 0; kind < ExitKindCount; ++kind) {
        text.append(
            std::format("  {:>8} block(s) end in {}\n", survey.exits.at(kind), ExitNames.at(kind)));
    }
    text.append(std::format("indirect jumps: {} through BX, {} through a load, {} through a block "
                            "load, {} through arithmetic\n",
                            survey.indirect_through_register,
                            survey.indirect_through_load,
                            survey.indirect_through_block_load,
                            survey.indirect_through_arithmetic));
    text.append(std::format("{} switch case(s)\n", survey.switch_cases));
    text.append(std::format("{} function(s) with a trap, {} at the size limit, {} without code\n",
                            survey.functions_with_a_trap,
                            survey.functions_at_the_limit,
                            survey.functions_without_code));
    constexpr std::size_t TrapsListed = 24;
    for (std::size_t index = 0; index < survey.trap_addresses.size() && index < TrapsListed;
         ++index) {
        text.append(std::format("  trap at {:08X}\n", survey.trap_addresses.at(index)));
    }
    return text;
}

}  // namespace hexlith::application
