#ifndef HEXLITH_APPLICATION_GOLDEN_TRACE_H
#define HEXLITH_APPLICATION_GOLDEN_TRACE_H

#include "application/boot.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hexlith::application {

// A golden trace: the sequence of service requests and module loads a run
// makes, one line each, with the frame it happened in. hexlith-boot writes
// one, and Azahar writes the same lines once scripts/azahar/golden-trace.patch
// is applied to it, so scripts/compare_golden_trace.py can find the first
// place the two disagree. docs/golden-traces.md describes the format and what
// the comparison does and does not mean.

// The first line of every golden trace, which names the format.
std::string GoldenTraceHeader();

std::string GoldenRequestLine(std::uint64_t frame, std::string_view service, std::uint32_t header);
std::string GoldenModuleLine(std::uint64_t frame,
                             const domain::services::RelocatableObjects::ModuleEvent& event);

// Watches the title and hands every line to the sink as it happens, the
// header first.
void RecordGoldenTrace(BootedTitle& title, std::function<void(std::string_view)> sink);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_GOLDEN_TRACE_H
