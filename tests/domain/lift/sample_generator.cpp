// Writes the C++ for the sample functions, for the build to compile into the
// test that runs it. Usage: sample_generator OUTPUT.cpp [CODE-DIGEST]
//
// With a digest the file also exports the two functions a lifted library does,
// so that it can be built as one and opened the way the real one is.
#include "application/lift_emit.h"
#include "domain/lift/control_flow.h"
#include "domain/lift/emit.h"

#include "sample_code.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace lift = hexlith::domain::lift;
namespace arm = hexlith::domain::arm;
namespace analysis = hexlith::domain::analysis;
namespace sample = hexlith::domain::lift::test::sample;

std::vector<std::uint8_t> Bytes() {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : sample::Code) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
    if (arguments.size() < 2 || arguments.size() > 3) {
        std::fputs("usage: sample_generator OUTPUT.cpp [CODE-DIGEST]\n", stderr);
        return 2;
    }

    const std::vector<std::uint8_t> bytes = Bytes();
    const lift::CodeImage image{.bytes = bytes, .base = sample::Base};
    const std::vector<std::uint32_t> entries = {sample::At(sample::SumIndex),
                                                sample::At(sample::CallerIndex),
                                                sample::At(sample::SwitcherIndex),
                                                sample::At(sample::MixedIndex),
                                                sample::At(sample::PatchedIndex)};
    const std::unordered_set<std::uint32_t> entry_set{entries.begin(), entries.end()};
    const std::vector<analysis::Region> regions = {
        analysis::Region{.address = sample::At(sample::TableIndex),
                         .size = sample::TableEntries * sample::Word,
                         .kind = analysis::RegionKind::JumpTable,
                         .instruction_set = std::nullopt,
                         .source = analysis::Provenance::Heuristic}};
    const lift::FlowContext flow{.entries = &entry_set, .regions = regions};

    lift::EmitContext context;
    context.function_at = [&entry_set](std::uint32_t address,
                                       arm::InstructionSet set) -> std::optional<std::string> {
        if (set == arm::InstructionSet::Arm && entry_set.contains(address)) {
            return lift::DefaultFunctionName(address, set);
        }
        return std::nullopt;
    };

    context.relocated = [](std::uint32_t address) {
        return address == sample::At(sample::RelocatedIndex);
    };

    std::ofstream out{arguments[1]};
    out << "#include \"domain/lift/runtime.h\"\n\n"
           "namespace hexlith::domain::lift::sample_lifted {\n\n"
           "namespace arm = hexlith::domain::arm;\n"
           "namespace memory = hexlith::domain::memory;\n"
           "namespace primitives = hexlith::domain::arm::primitives;\n\n";
    for (const std::uint32_t entry : entries) {
        out << lift::DeclarationOf(lift::DefaultFunctionName(entry, arm::InstructionSet::Arm))
            << '\n';
    }
    out << '\n';

    std::string registration = "void Register(runtime::EntryTable& table) {\n";
    for (const std::uint32_t entry : entries) {
        const lift::Function function =
            lift::BuildFunction(image, entry, arm::InstructionSet::Arm, flow);
        const std::string name = lift::DefaultFunctionName(entry, arm::InstructionSet::Arm);
        const lift::EmittedFunction emitted = lift::EmitFunction(function, name, context);
        out << emitted.text << '\n';
        for (const std::uint32_t address : emitted.entries) {
            registration += std::format("    table.Add(0x{:08X}U, false, &{});\n", address, name);
        }
    }
    registration += "}\n\n}  // namespace hexlith::domain::lift::sample_lifted\n";
    out << registration;
    if (arguments.size() == 3) {
        // The version is the lifter's, and the library is refused when it is not
        // the one that is running, so the tests that open it say it by name.
        out << std::format(
            "\nextern \"C\" {{\n"
            "const char* hexlith_lifted_manifest() {{ return \"lifter={} code={} functions=5\"; }}\n"
            "void hexlith_lifted_register(void* table) {{\n"
            "    hexlith::domain::lift::sample_lifted::Register(\n"
            "        *static_cast<hexlith::domain::lift::runtime::EntryTable*>(table));\n"
            "}}\n}}\n",
            hexlith::application::LifterVersion,
            arguments[2]);
    }
    return out.good() ? 0 : 1;
}
