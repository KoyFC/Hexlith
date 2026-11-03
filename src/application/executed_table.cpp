#include "application/executed_table.h"

#include "domain/analysis/record_text.h"

#include <algorithm>
#include <array>

namespace hexlith::application {

std::string RenderExecutedTable(const domain::arm::ExecutedAddresses& executed,
                                std::uint32_t first,
                                std::uint32_t end) {
    namespace analysis = domain::analysis;

    std::string text = analysis::RenderComment(
        "Executed: the instructions a run executed, by address, with how many times.\n"
        "Written by hexlith-boot and never edited by hand: it is what one run saw, not\n"
        "what the analysis concluded.");
    text += analysis::RenderVersionLine();

    for (const domain::arm::ExecutedAddresses::Entry& entry : executed.Entries()) {
        if (entry.address < first || entry.address >= end) {
            continue;
        }
        const std::array<analysis::RecordField, 2> fields = {
            analysis::RecordField{.key = "isa", .value = entry.thumb ? "thumb" : "arm"},
            analysis::RecordField{.key = "count", .value = std::to_string(entry.count)}};
        text += analysis::RenderRecord(entry.address, fields);
    }
    return text;
}

void ExecutionRecorder::BeforeInstruction(std::uint32_t address, bool thumb) {
    for (const Resident& each : residents_) {
        if (address >= each.first && address < each.end) {
            each.record->executed.BeforeInstruction(address - each.base, thumb);
            return;
        }
    }
    main_.BeforeInstruction(address, thumb);
}

void ExecutionRecorder::ModuleLoaded(std::string_view name,
                                     std::uint32_t address,
                                     std::uint32_t code_offset,
                                     std::uint32_t code_size) {
    auto [at, added] = modules_.try_emplace(std::string{name});
    static_cast<void>(added);
    ModuleRecord& record = at->second;
    record.code_offset = code_offset;
    record.code_size = code_size;
    residents_.push_back(Resident{.name = std::string{name},
                                  .base = address,
                                  .first = address + code_offset,
                                  .end = address + code_offset + code_size,
                                  .record = &record});
}

void ExecutionRecorder::ModuleUnloaded(std::string_view name, std::uint32_t address) {
    std::erase_if(residents_,
                  [&](const Resident& each) { return each.base == address && each.name == name; });
}

}  // namespace hexlith::application
