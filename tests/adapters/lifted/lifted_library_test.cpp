#include "adapters/lifted/lifted_library.h"
#include "domain/lift/engine.h"
#include "domain/lift/runtime.h"
#include "domain/memory/process_memory.h"

#include "sample_code.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace hexlith::adapters::lifted {
namespace {

namespace sample = domain::lift::test::sample;

std::string SampleDigest() {
    std::string digest(64, '0');
    return digest;
}

TEST(LiftedLibrary, OpensALibraryLiftedFromTheCodeItIsAskedFor) {
    const auto library = LiftedLibrary::Open(HEXLITH_SAMPLE_LIBRARY, SampleDigest());
    ASSERT_TRUE(library.Ok()) << library.Failure().Message();
    EXPECT_NE(library.Value()->Manifest().find("lifter="), std::string::npos);
    EXPECT_NE(library.Value()->Entries().Find(sample::At(sample::SumIndex), false), nullptr);
}

TEST(LiftedLibrary, RefusesALibraryLiftedFromOtherCode) {
    const auto library = LiftedLibrary::Open(HEXLITH_SAMPLE_LIBRARY, std::string(64, 'f'));
    ASSERT_FALSE(library.Ok());
    EXPECT_NE(library.Failure().Message().find("other code"), std::string::npos);
}

TEST(LiftedLibrary, RefusesAFileThatIsNotThere) {
    EXPECT_FALSE(LiftedLibrary::Open("/nonexistent/libx.so", SampleDigest()).Ok());
}

TEST(LiftedLibrary, ItsFunctionsRunThroughTheEngine) {
    const auto library = LiftedLibrary::Open(HEXLITH_SAMPLE_LIBRARY, SampleDigest());
    ASSERT_TRUE(library.Ok());

    domain::memory::ProcessMemory memory;
    constexpr std::uint32_t Page = 0x1000;
    const auto code = domain::memory::Backing::Allocate(2 * Page);
    for (std::size_t index = 0; index < sample::Code.size(); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            code->Bytes()[(index * 4) + byte] =
                static_cast<std::uint8_t>(sample::Code.at(index) >> (8U * byte));
        }
    }
    ASSERT_TRUE(memory.Map(sample::Base, 2 * Page, code, 0, domain::memory::Access::ReadExecute));

    domain::arm::CpuState state;
    state.registers.at(domain::arm::ProgramCounter) = sample::At(sample::SumIndex);
    state.registers.at(1) = 10;
    state.registers.at(domain::arm::LinkRegister) = sample::At(sample::SentinelIndex);

    const domain::lift::Engine engine{library.Value()->Entries()};
    std::uint64_t executed = 0;
    const domain::arm::Stop stop = engine.Run(state, memory, 10000, executed);
    EXPECT_EQ(stop.reason, domain::arm::StopReason::SupervisorCall);
    EXPECT_EQ(state.registers.at(0), 55U);
    EXPECT_EQ(engine.Counted().interpreted, 1U) << "only the supervisor call at the end";
}

}  // namespace
}  // namespace hexlith::adapters::lifted
