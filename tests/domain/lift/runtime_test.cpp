#include "domain/lift/runtime.h"

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace hexlith::domain::lift::runtime
