#include "domain/analysis/provenance.h"

#include <array>
#include <gtest/gtest.h>
#include <string>

namespace hexlith::domain::analysis {
namespace {

constexpr std::array<Provenance, 6> AllSources = {Provenance::CrsExport,
                                                  Provenance::ExHeader,
                                                  Provenance::Relocation,
                                                  Provenance::Ghidra,
                                                  Provenance::Heuristic,
                                                  Provenance::Manual};

TEST(Provenance, EverySourceRoundTripsThroughItsSpelling) {
    for (const Provenance source : AllSources) {
        const std::string_view spelling = Describe(source);
        EXPECT_NE(spelling, "unknown") << "a source has no spelling of its own";
        EXPECT_EQ(ParseProvenance(spelling), source) << spelling;
    }
}

TEST(Provenance, RefusesSomethingThatIsNotASource) {
    EXPECT_FALSE(ParseProvenance("guess").has_value());
    EXPECT_FALSE(ParseProvenance("").has_value());
    EXPECT_FALSE(ParseProvenance("Manual").has_value());
}

TEST(Provenance, HumanWorkIsNotDerivedFromTheImage) {
    EXPECT_FALSE(IsDerivedFromTheImage(Provenance::Manual));
    EXPECT_FALSE(IsDerivedFromTheImage(Provenance::Ghidra));
}

TEST(Provenance, AnalysisOutputIsDerivedFromTheImage) {
    EXPECT_TRUE(IsDerivedFromTheImage(Provenance::CrsExport));
    EXPECT_TRUE(IsDerivedFromTheImage(Provenance::ExHeader));
    EXPECT_TRUE(IsDerivedFromTheImage(Provenance::Relocation));
    EXPECT_TRUE(IsDerivedFromTheImage(Provenance::Heuristic));
}

}  // namespace
}  // namespace hexlith::domain::analysis
