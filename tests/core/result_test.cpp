#include "core/result.h"

#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace hexlith::core {
namespace {

constexpr int ParsedField = 42;
constexpr int Fallback = 7;

Result<int> Parse(bool succeed) {
    if (!succeed) {
        return Error{"offset 0x10 is past the end"};
    }
    return ParsedField;
}

TEST(Error, WithContextPrependsTheCaller) {
    const Error inner{"offset 0x1b0 is past the end of 0x200 bytes"};
    const Error outer = inner.WithContext("NCCH header");
    EXPECT_EQ(outer.Message(), "NCCH header: offset 0x1b0 is past the end of 0x200 bytes");
}

TEST(Error, ContextNests) {
    const Error message = Error{"truncated"}.WithContext("ExeFS header").WithContext("partition 0");
    EXPECT_EQ(message.Message(), "partition 0: ExeFS header: truncated");
}

TEST(Result, CarriesAValue) {
    const Result<int> result = Parse(true);
    ASSERT_TRUE(result.Ok());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.Value(), ParsedField);
}

TEST(Result, CarriesAFailure) {
    const Result<int> result = Parse(false);
    ASSERT_FALSE(result.Ok());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.Failure().Message(), "offset 0x10 is past the end");
}

TEST(Result, ValueOrSubstitutesOnFailure) {
    EXPECT_EQ(Parse(true).ValueOr(Fallback), ParsedField);
    EXPECT_EQ(Parse(false).ValueOr(Fallback), Fallback);
}

TEST(Result, ReadingTheValueOfAFailureThrows) {
    const Result<int> result = Parse(false);
    EXPECT_THROW((void)result.Value(), std::logic_error);
}

TEST(Result, ReadingTheFailureOfAValueThrows) {
    const Result<int> result = Parse(true);
    EXPECT_THROW((void)result.Failure(), std::logic_error);
}

TEST(Result, FailureMessageNamesTheOriginalErrorWhenMisused) {
    const Result<int> result = Parse(false);
    try {
        (void)result.Value();
        FAIL() << "expected Value to throw";
    } catch (const std::logic_error& thrown) {
        EXPECT_NE(std::string{thrown.what()}.find("offset 0x10 is past the end"),
                  std::string::npos);
    }
}

TEST(Result, HoldsAMoveOnlyValue) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(ParsedField);
    ASSERT_TRUE(result.Ok());
    EXPECT_EQ(*result.Value(), ParsedField);
}

TEST(Status, DefaultsToSuccess) {
    const Status status;
    EXPECT_TRUE(status.Ok());
}

TEST(Status, CarriesAFailure) {
    const Status status = Error{"the partition is encrypted"};
    ASSERT_FALSE(status.Ok());
    EXPECT_EQ(status.Failure().Message(), "the partition is encrypted");
}

}  // namespace
}  // namespace hexlith::core
