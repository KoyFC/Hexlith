#include "domain/lift/runtime.h"

#include <gtest/gtest.h>
#include <optional>
#include <string_view>

namespace hexlith::domain::lift::runtime {
namespace {

Pc Nowhere(Context& /*context*/, Pc pc) {
    return pc;
}

TEST(EntryTable, CoversNothingUntilAnEntryIsAdded) {
    const EntryTable table;
    EXPECT_FALSE(table.MayCover(0));
    EXPECT_FALSE(table.MayCover(0x00100000));
}

TEST(EntryTable, MayCoverTheSpanOfItsEntriesInEitherInstructionSet) {
    EntryTable table;
    table.Add(0x00100000, false, Nowhere);
    table.Add(0x00200000, true, Nowhere);

    EXPECT_TRUE(table.MayCover(0x00100000));
    EXPECT_TRUE(table.MayCover(0x00180000));
    EXPECT_TRUE(table.MayCover(0x00200000));
    EXPECT_FALSE(table.MayCover(0x000FFFFC));
    EXPECT_FALSE(table.MayCover(0x00200002));
    EXPECT_EQ(table.Find(0x00180000, false), nullptr);
}

// Leaves the base it was run against and the address it was given where the test
// can read them, in the guest's registers.
constexpr std::size_t BaseRegister = 0;
constexpr std::size_t AddressRegister = 1;

Pc RecordsItsBase(Context& context, Pc pc) {
    context.cpu->registers.at(BaseRegister) = context.base;
    context.cpu->registers.at(AddressRegister) = pc;
    return 0x1234;
}

// A module's table is keyed by offset from the module's start.
constexpr std::uint32_t CodeOffset = 0x180;
constexpr std::uint32_t CodeSize = 0x1000;
constexpr std::uint32_t EntryOffset = 0x400;
constexpr std::string_view Digest = "aa";

EntryTable ModuleTable() {
    EntryTable table;
    table.Add(EntryOffset, false, RecordsItsBase);
    return table;
}

// The base and the function of the module code at an address, if there is any.
std::optional<std::uint32_t> BaseAt(const LoadedModules& modules, Pc address) {
    const std::optional<LoadedModules::Found> found = modules.Find(address, false);
    if (!found) {
        return std::nullopt;
    }
    return found->base;
}

Function FunctionAt(const LoadedModules& modules, Pc address) {
    const std::optional<LoadedModules::Found> found = modules.Find(address, false);
    return found ? found->function : nullptr;
}

TEST(LoadedModules, FindsALoadedModulesFunctionByItsOffsetAndGivesItsBase) {
    const EntryTable table = ModuleTable();
    LoadedModules modules;
    modules.Offer("DllA", Digest, table);
    ASSERT_EQ(modules.Loaded("DllA", 0x006A5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::Lifted);

    EXPECT_EQ(FunctionAt(modules, 0x006A5000 + EntryOffset), &RecordsItsBase);
    EXPECT_EQ(BaseAt(modules, 0x006A5000 + EntryOffset), 0x006A5000U);
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, true).has_value())
        << "the other instruction set";
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset + 4, false).has_value()) << "not an entry";
}

TEST(LoadedModules, OnlyCoversTheCodeSegmentOfWhatIsLoaded) {
    const EntryTable table = ModuleTable();
    LoadedModules modules;
    modules.Offer("DllA", Digest, table);
    ASSERT_EQ(modules.Loaded("DllA", 0x006A5000, 0x100, 0x200, Digest),
              LoadedModules::Outcome::Lifted);
    // The offset is within the module but the code segment ends before it.
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, false).has_value());
}

TEST(LoadedModules, ForgetsAModuleWhenItIsUnloadedAndFindsItAgainWhereItIsReloaded) {
    const EntryTable table = ModuleTable();
    LoadedModules modules;
    modules.Offer("DllA", Digest, table);
    ASSERT_EQ(modules.Loaded("DllA", 0x006A5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::Lifted);
    modules.Unloaded("DllA", 0x006A5000);
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, false).has_value());

    ASSERT_EQ(modules.Loaded("DllA", 0x007E5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::Lifted);
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, false).has_value());
    EXPECT_EQ(BaseAt(modules, 0x007E5000 + EntryOffset), 0x007E5000U);
}

TEST(LoadedModules, AModuleLoadedTwiceAtOnceIsTwoRangesOverOneTable) {
    const EntryTable table = ModuleTable();
    LoadedModules modules;
    modules.Offer("DllA", Digest, table);
    ASSERT_EQ(modules.Loaded("DllA", 0x006A5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::Lifted);
    ASSERT_EQ(modules.Loaded("DllA", 0x007E5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::Lifted);
    EXPECT_EQ(BaseAt(modules, 0x006A5000 + EntryOffset), 0x006A5000U);
    EXPECT_EQ(BaseAt(modules, 0x007E5000 + EntryOffset), 0x007E5000U);

    modules.Unloaded("DllA", 0x006A5000);
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, false).has_value());
    EXPECT_TRUE(modules.Find(0x007E5000 + EntryOffset, false).has_value())
        << "an unload names the address, and the other copy stays";
}

TEST(LoadedModules, RunsAModuleInterpretedWhenItsBytesAreNotThoseThatWereLifted) {
    const EntryTable table = ModuleTable();
    LoadedModules modules;
    modules.Offer("DllA", Digest, table);
    EXPECT_EQ(modules.Loaded("DllA", 0x006A5000, CodeOffset, CodeSize, "bb"),
              LoadedModules::Outcome::DigestDiffers);
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, false).has_value());
}

TEST(LoadedModules, RunsAModuleInterpretedWhenNothingWasLiftedForIt) {
    LoadedModules modules;
    EXPECT_EQ(modules.Loaded("DllB", 0x006A5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::NotLifted);
    EXPECT_FALSE(modules.Find(0x006A5000 + EntryOffset, false).has_value());
}

TEST(Dispatch, EntersAModulesFunctionWithItsBaseAndPutsTheCallersBackAfterwards) {
    const EntryTable main_table;
    const EntryTable table = ModuleTable();
    LoadedModules modules;
    modules.Offer("DllA", Digest, table);
    ASSERT_EQ(modules.Loaded("DllA", 0x006A5000, CodeOffset, CodeSize, Digest),
              LoadedModules::Outcome::Lifted);

    arm::CpuState cpu;
    Context context;
    context.cpu = &cpu;
    context.entries = &main_table;
    context.modules = &modules;
    context.base = 0x007E5000;  // the caller is another module

    EXPECT_EQ(Dispatch(context, 0x006A5000 + EntryOffset), 0x1234U);
    EXPECT_EQ(cpu.registers.at(BaseRegister), 0x006A5000U);
    EXPECT_EQ(cpu.registers.at(AddressRegister), 0x006A5000 + EntryOffset);
    EXPECT_EQ(context.base, 0x007E5000U);
}

TEST(Dispatch, AnswersTheAddressWhenNothingIsLiftedThere) {
    const EntryTable main_table;
    const LoadedModules modules;
    arm::CpuState cpu;
    Context context;
    context.cpu = &cpu;
    context.entries = &main_table;
    context.modules = &modules;
    EXPECT_EQ(Dispatch(context, 0x006A5400), 0x006A5400U);
    context.modules = nullptr;
    EXPECT_EQ(Dispatch(context, 0x006A5400), 0x006A5400U) << "and without modules at all";
}

}  // namespace
}  // namespace hexlith::domain::lift::runtime
