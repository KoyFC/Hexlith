#include "application/executed_table.h"

#include "domain/analysis/record_text.h"

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

}  // namespace hexlith::application
