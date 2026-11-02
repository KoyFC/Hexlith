#include "application/lift_survey.h"

#include "domain/analysis/record_text.h"
#include "domain/analysis/tables.h"
#include "domain/lift/lower.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
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

// Every distinct instruction the functions hold, once, wherever it is reached from.
using Instructions = std::map<std::uint32_t, lift::PlacedInstruction>;

void Collect(Instructions& instructions, const lift::Function& function) {
    for (const lift::BasicBlock& block : function.blocks) {
        for (const lift::PlacedInstruction& placed : block.instructions) {
            instructions.emplace(placed.address, placed);
        }
    }
}

// How many times a run executed each address, by instruction set.
core::Result<std::map<std::uint32_t, std::uint64_t>> ExecutedCounts(std::string_view text) {
    std::map<std::uint32_t, std::uint64_t> counts;
    if (text.empty()) {
        return counts;
    }
    const core::Result<analysis::RecordFile> parsed = analysis::ParseRecordText(text);
    if (!parsed) {
        return parsed.Failure();
    }
    for (const analysis::Record& record : parsed.Value().records) {
        const core::Result<std::string_view> count = analysis::RequiredField(record, "count");
        if (!count) {
            return count.Failure();
        }
        counts[record.address] += std::stoull(std::string{count.Value()});
    }
    return counts;
}

void Measure(LiftSurvey& survey,
             const Instructions& instructions,
             const std::map<std::uint32_t, std::uint64_t>& executed) {
    std::map<domain::arm::Operation, OperationCoverage> by_operation;
    for (const auto& [address, placed] : instructions) {
        const bool interpreted = lift::Lower(placed).IsInterpreted();
        const auto ran = executed.find(address);
        const std::uint64_t times = ran == executed.end() ? 0 : ran->second;

        OperationCoverage& entry = by_operation[placed.instruction.operation];
        entry.operation = placed.instruction.operation;
        ++entry.instructions;
        entry.executed += times;
        ++survey.distinct_instructions;
        survey.executed_total += times;
        if (interpreted) {
            ++entry.interpreted;
            entry.executed_interpreted += times;
            ++survey.distinct_interpreted;
            survey.executed_interpreted += times;
        }
    }
    for (const auto& [operation, entry] : by_operation) {
        survey.coverage.push_back(entry);
    }
    std::ranges::sort(survey.coverage, [](const OperationCoverage& l, const OperationCoverage& r) {
        if (l.executed_interpreted != r.executed_interpreted) {
            return l.executed_interpreted > r.executed_interpreted;
        }
        return l.interpreted > r.interpreted;
    });
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

    const core::Result<std::map<std::uint32_t, std::uint64_t>> executed =
        ExecutedCounts(request.executed);
    if (!executed) {
        return executed.Failure().WithContext("executed.txt");
    }

    LiftSurvey survey;
    Instructions instructions;
    for (const analysis::FunctionRange& function : functions.Value()) {
        const lift::Function built =
            lift::BuildFunction(image, function.address, function.instruction_set, context);
        Add(survey, built);
        Collect(instructions, built);
    }
    Measure(survey, instructions, executed.Value());
    return survey;
}

namespace {

std::string RenderCoverage(const LiftSurvey& survey) {
    std::string text;
    const auto percent = [](double part, double whole) {
        return whole == 0.0 ? 0.0 : 100.0 * part / whole;
    };
    text.append(std::format("{} distinct instruction(s), {} left to the interpreter ({:.1f}%)\n",
                            survey.distinct_instructions,
                            survey.distinct_interpreted,
                            percent(static_cast<double>(survey.distinct_interpreted),
                                    static_cast<double>(survey.distinct_instructions))));
    if (survey.executed_total != 0) {
        text.append(std::format("{} executed, {} of them by the interpreter ({:.2f}%)\n",
                                survey.executed_total,
                                survey.executed_interpreted,
                                percent(static_cast<double>(survey.executed_interpreted),
                                        static_cast<double>(survey.executed_total))));
    }
    constexpr std::size_t Listed = 30;
    text.append("operations left to the interpreter, by how much they run:\n");
    for (std::size_t index = 0; index < survey.coverage.size() && index < Listed; ++index) {
        const OperationCoverage& entry = survey.coverage.at(index);
        if (entry.interpreted == 0) {
            break;
        }
        text.append(std::format("  {:<12} {:>8} of {:>8} distinct, {:>12} executed\n",
                                domain::arm::Mnemonic(entry.operation),
                                entry.interpreted,
                                entry.instructions,
                                entry.executed_interpreted));
    }
    return text;
}

}  // namespace

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
    text.append(RenderCoverage(survey));
    constexpr std::size_t TrapsListed = 24;
    for (std::size_t index = 0; index < survey.trap_addresses.size() && index < TrapsListed;
         ++index) {
        text.append(std::format("  trap at {:08X}\n", survey.trap_addresses.at(index)));
    }
    return text;
}

}  // namespace hexlith::application
