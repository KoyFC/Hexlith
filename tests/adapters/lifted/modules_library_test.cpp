#include "adapters/lifted/modules_library.h"
#include "domain/lift/engine.h"
#include "domain/lift/runtime.h"
#include "domain/memory/process_memory.h"

#include "sample_code.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace hexlith::adapters::lifted {
namespace {

namespace sample = domain::lift::test::sample;
namespace runtime = domain::lift::runtime;

constexpr std::uint32_t Base = 0x00400000;
constexpr std::uint32_t CodeSize = static_cast<std::uint32_t>(sample::Code.size() * 4);

// The digest the sample library says its module was lifted from.
std::string SampleDigest() {
    std::string digest(64, '0');
    return digest;
}

// The sample's code mapped where a module at `base` has it.
void MapSample(domain::memory::ProcessMemory& memory, std::uint32_t base) {
    constexpr std::uint32_t Page = 0x1000;
    const auto code = domain::memory::Backing::Allocate(2 * Page);
    for (std::size_t index = 0; index < sample::Code.size(); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            code->Bytes()[(index * 4) + byte] =
                static_cast<std::uint8_t>(sample::Code.at(index) >> (8U * byte));
        }
    }
    ASSERT_TRUE(
        memory.Map(base + sample::Base, 2 * Page, code, 0, domain::memory::Access::ReadExecute));
}

TEST(ModulesLibrary, OpensALibraryOfModulesAndOffersThemToARun) {
    const auto library = ModulesLibrary::Open(HEXLITH_SAMPLE_LIBRARY);
    ASSERT_TRUE(library.Ok()) << library.Failure().Message();
    EXPECT_EQ(library.Value()->Count(), 1U);
    EXPECT_NE(library.Value()->Manifest().find("modules=1"), std::string::npos);

    runtime::LoadedModules modules;
    library.Value()->OfferTo(modules);
    EXPECT_EQ(modules.Loaded("Sample", Base, sample::Base, CodeSize, SampleDigest()),
              runtime::LoadedModules::Outcome::Lifted);
    EXPECT_TRUE(modules.Find(Base + sample::At(sample::SumIndex), false).has_value());
    EXPECT_EQ(modules.Loaded("Other", Base, sample::Base, CodeSize, SampleDigest()),
              runtime::LoadedModules::Outcome::NotLifted);
}

TEST(ModulesLibrary, LeavesAModuleToTheInterpreterWhenItsCodeIsNotWhatWasLifted) {
    const auto library = ModulesLibrary::Open(HEXLITH_SAMPLE_LIBRARY);
    ASSERT_TRUE(library.Ok());
    runtime::LoadedModules modules;
    library.Value()->OfferTo(modules);
    EXPECT_EQ(modules.Loaded("Sample", Base, sample::Base, CodeSize, std::string(64, 'f')),
              runtime::LoadedModules::Outcome::DigestDiffers);
    EXPECT_FALSE(modules.Find(Base + sample::At(sample::SumIndex), false).has_value());
}

TEST(ModulesLibrary, RefusesALibraryFromAnotherLifter) {
    const auto library = ModulesLibrary::Open(HEXLITH_STALE_MODULES_LIBRARY);
    ASSERT_FALSE(library.Ok());
    EXPECT_NE(library.Failure().Message().find("another lifter"), std::string::npos);
}

TEST(ModulesLibrary, RefusesALibraryThatIsNotOneOfModulesAndAFileThatIsNotThere) {
    EXPECT_FALSE(ModulesLibrary::Open("libm.so.6").Ok());
    EXPECT_FALSE(ModulesLibrary::Open("/nonexistent/libx.so").Ok());
}

TEST(ModulesLibrary, ItsFunctionsRunThroughTheEngineWhereTheModuleIs) {
    const auto library = ModulesLibrary::Open(HEXLITH_SAMPLE_LIBRARY);
    ASSERT_TRUE(library.Ok());
    runtime::LoadedModules modules;
    library.Value()->OfferTo(modules);
    ASSERT_EQ(modules.Loaded("Sample", Base, sample::Base, CodeSize, SampleDigest()),
              runtime::LoadedModules::Outcome::Lifted);

    domain::memory::ProcessMemory memory;
    MapSample(memory, Base);

    domain::arm::CpuState state;
    state.registers.at(domain::arm::ProgramCounter) = Base + sample::At(sample::SumIndex);
    state.registers.at(1) = 10;
    state.registers.at(domain::arm::LinkRegister) = Base + sample::At(sample::SentinelIndex);

    const runtime::EntryTable none;
    const domain::lift::Engine engine{none, &modules};
    std::uint64_t executed = 0;
    const domain::arm::Stop stop = engine.Run(state, memory, 10000, executed);
    EXPECT_EQ(stop.reason, domain::arm::StopReason::SupervisorCall);
    EXPECT_EQ(state.registers.at(0), 55U);
    EXPECT_EQ(engine.Counted().interpreted, 1U) << "only the supervisor call at the end";
    EXPECT_GT(engine.Counted().module_entries, 0U);
}

}  // namespace
}  // namespace hexlith::adapters::lifted
