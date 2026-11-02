#include "application/command_line.h"
#include "application/lift_survey.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

namespace hexlith::application {
namespace {

namespace lift = domain::lift;

constexpr std::uint32_t Base = 0x00100000;

constexpr std::uint32_t ArmPush = 0xE92D4010;        // push {r4, lr}
constexpr std::uint32_t ArmMove = 0xE1A01002;        // mov r1, r2
constexpr std::uint32_t ArmPop = 0xE8BD8010;         // pop {r4, pc}
constexpr std::uint32_t ArmBranchToR3 = 0xE12FFF13;  // bx r3
constexpr std::uint32_t ArmUndefined = 0xE7F000F0;   // udf #0

std::vector<std::uint8_t> Code(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : words) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }
    return bytes;
}

constexpr std::string_view Regions =
    "format version=1\n"
    "0x00100000 size=0x00000020 kind=code isa=arm source=heuristic\n";

TEST(SurveyControlFlow, CountsFunctionsBlocksAndHowTheyEnd) {
    // Two functions: one that returns, and one that jumps through a register.
    const std::vector<std::uint8_t> code = Code({ArmPush, ArmMove, ArmPop, ArmBranchToR3});
    const std::string_view functions = "format version=1\n"
                                       "0x00100000 size=0x0000000C isa=arm source=heuristic\n"
                                       "0x0010000C size=0x00000004 isa=arm source=heuristic\n";

    const core::Result<LiftSurvey> survey = SurveyControlFlow(LiftSurveyRequest{
        .code = code, .base = Base, .functions = functions, .regions = Regions, .executed = {}});
    ASSERT_TRUE(survey.Ok()) << survey.Failure().Message();

    EXPECT_EQ(survey.Value().functions, 2U);
    EXPECT_EQ(survey.Value().arm_functions, 2U);
    EXPECT_EQ(survey.Value().blocks, 2U);
    EXPECT_EQ(survey.Value().Exits(lift::ExitKind::Return), 1U);
    EXPECT_EQ(survey.Value().Exits(lift::ExitKind::IndirectJump), 1U);
    EXPECT_EQ(survey.Value().indirect_through_register, 1U);
    EXPECT_EQ(survey.Value().largest_function, 3U);
}

TEST(SurveyControlFlow, ReportsWhereTheControlFlowCannotBeFollowed) {
    const std::vector<std::uint8_t> code = Code({ArmMove, ArmUndefined});
    const std::string_view functions = "format version=1\n"
                                       "0x00100000 size=0x00000008 isa=arm source=heuristic\n";

    const core::Result<LiftSurvey> survey = SurveyControlFlow(LiftSurveyRequest{
        .code = code, .base = Base, .functions = functions, .regions = Regions, .executed = {}});
    ASSERT_TRUE(survey.Ok());
    EXPECT_EQ(survey.Value().functions_with_a_trap, 1U);
    ASSERT_EQ(survey.Value().trap_addresses.size(), 1U);
    EXPECT_EQ(survey.Value().trap_addresses.front(), Base + 4);
}

TEST(SurveyControlFlow, RefusesTablesItCannotRead) {
    const std::vector<std::uint8_t> code = Code({ArmPop});
    const core::Result<LiftSurvey> survey =
        SurveyControlFlow(LiftSurveyRequest{.code = code,
                                            .base = Base,
                                            .functions = "not a table",
                                            .regions = Regions,
                                            .executed = {}});
    EXPECT_FALSE(survey.Ok());
}

TEST(RenderLiftSurvey, NamesTheNumbersWorthWatching) {
    const std::vector<std::uint8_t> code = Code({ArmPop});
    const std::string_view functions = "format version=1\n"
                                       "0x00100000 size=0x00000004 isa=arm source=heuristic\n";
    const core::Result<LiftSurvey> survey = SurveyControlFlow(LiftSurveyRequest{
        .code = code, .base = Base, .functions = functions, .regions = Regions, .executed = {}});
    ASSERT_TRUE(survey.Ok());
    const std::string text = RenderLiftSurvey(survey.Value());
    EXPECT_NE(text.find("1 function(s): 1 ARM, 0 Thumb"), std::string::npos);
    EXPECT_NE(text.find("end in return"), std::string::npos);
}

TEST(ParseLiftCommandLine, TakesTheSurveyAndTheWorkDirectory) {
    const std::vector<std::string_view> arguments = {"--work-dir", "/work", "--survey"};
    const core::Result<LiftOptions> options = ParseLiftCommandLine(arguments);
    ASSERT_TRUE(options.Ok()) << options.Failure().Message();
    EXPECT_TRUE(options.Value().survey);
    EXPECT_EQ(options.Value().work_directory, "/work");
}

TEST(ParseLiftCommandLine, RefusesWhatItDoesNotKnow) {
    const std::vector<std::string_view> unknown = {"--emit"};
    EXPECT_FALSE(ParseLiftCommandLine(unknown).Ok());
    const std::vector<std::string_view> loose = {"/work"};
    EXPECT_FALSE(ParseLiftCommandLine(loose).Ok());
    const std::vector<std::string_view> help = {"--help"};
    EXPECT_TRUE(ParseLiftCommandLine(help).Value().help);
}

}  // namespace
}  // namespace hexlith::application
