#include "domain/analysis/record_text.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::analysis {
namespace {

RecordFile ParseOrFail(std::string_view text) {
    const core::Result<RecordFile> parsed = ParseRecordText(text);
    EXPECT_TRUE(parsed.Ok()) << parsed.Failure().Message();
    return parsed.Ok() ? parsed.Value() : RecordFile{};
}

std::string FailureOf(std::string_view text) {
    const core::Result<RecordFile> parsed = ParseRecordText(text);
    EXPECT_FALSE(parsed.Ok()) << "the text was accepted";
    return parsed.Ok() ? std::string{} : parsed.Failure().Message();
}

TEST(ParseRecordText, ReadsAnAddressAndItsFields) {
    const RecordFile file =
        ParseOrFail("format version=1\n"
                    "0x0012A4B0 size=0x000000C4 kind=func name=FieldMain source=crs-export\n");

    ASSERT_EQ(file.records.size(), 1U);
    const Record& record = file.records.front();
    EXPECT_EQ(record.address, 0x0012A4B0U);
    EXPECT_EQ(record.line, 2U);
    ASSERT_EQ(record.fields.size(), 4U);
    EXPECT_EQ(record.fields[0].key, "size");
    EXPECT_EQ(record.fields[0].value, "0x000000C4");
    EXPECT_EQ(record.fields[3].key, "source");
    ASSERT_NE(record.Find("name"), nullptr);
    EXPECT_EQ(*record.Find("name"), "FieldMain");
    EXPECT_EQ(record.Find("isa"), nullptr);
}

TEST(ParseRecordText, IgnoresBlankLinesAndComments) {
    const RecordFile file = ParseOrFail("# a banner\n"
                                        "\n"
                                        "format version=1\n"
                                        "   # indented, with a 0x0 in it\n"
                                        "0x00000004 size=0x00000004 source=manual\n");
    ASSERT_EQ(file.records.size(), 1U);
    EXPECT_EQ(file.records.front().line, 5U);
}

TEST(ParseRecordText, AcceptsCarriageReturnsAndMissingFinalNewline) {
    const RecordFile file =
        ParseOrFail("format version=1\r\n0x00000008 size=0x00000004 source=manual");
    ASSERT_EQ(file.records.size(), 1U);
    EXPECT_EQ(*file.records.front().Find("source"), "manual");
}

TEST(ParseRecordText, AcceptsAnAddressWrittenShort) {
    const RecordFile file = ParseOrFail("format version=1\n0x100000 size=0x4 source=manual\n");
    ASSERT_EQ(file.records.size(), 1U);
    EXPECT_EQ(file.records.front().address, 0x00100000U);
}

TEST(ParseRecordText, AcceptsUpperCaseDigitsAndPrefix) {
    const RecordFile file = ParseOrFail("format version=1\n0XABCDEF01 size=0xff source=manual\n");
    ASSERT_EQ(file.records.size(), 1U);
    EXPECT_EQ(file.records.front().address, 0xABCDEF01U);
}

TEST(ParseRecordText, RequiresTheFormatLineBeforeAnyRecord) {
    EXPECT_NE(FailureOf("0x00000004 size=0x4 source=manual\n").find("before"), std::string::npos);
    EXPECT_NE(FailureOf("# only a comment\n").find("format version=1"), std::string::npos);
    EXPECT_NE(FailureOf("").find("format version=1"), std::string::npos);
}

TEST(ParseRecordText, RefusesAVersionItDoesNotKnow) {
    const std::string failure = FailureOf("format version=2\n");
    EXPECT_NE(failure.find("version 2"), std::string::npos) << failure;
    EXPECT_NE(failure.find("different version of Hexlith"), std::string::npos) << failure;
}

TEST(ParseRecordText, RefusesASecondFormatLine) {
    EXPECT_NE(FailureOf("format version=1\nformat version=1\n").find("twice"), std::string::npos);
}

TEST(ParseRecordText, RefusesAMalformedFormatLine) {
    EXPECT_FALSE(ParseRecordText("format\n").Ok());
    EXPECT_FALSE(ParseRecordText("format version=1 extra=2\n").Ok());
    EXPECT_FALSE(ParseRecordText("format revision=1\n").Ok());
    EXPECT_FALSE(ParseRecordText("format version=0x1\n").Ok());
    EXPECT_FALSE(ParseRecordText("format version=0\n").Ok());
}

TEST(ParseRecordText, RefusesALineThatDoesNotStartWithAnAddress) {
    EXPECT_NE(FailureOf("format version=1\n1234 size=0x4\n").find("is not an address"),
              std::string::npos);
    EXPECT_FALSE(ParseRecordText("format version=1\n0x123456789 size=0x4\n").Ok());
    EXPECT_FALSE(ParseRecordText("format version=1\n0x size=0x4\n").Ok());
    EXPECT_FALSE(ParseRecordText("format version=1\n0xZZ size=0x4\n").Ok());
}

TEST(ParseRecordText, NamesTheLineAFailureIsOn) {
    const std::string failure = FailureOf("format version=1\n"
                                          "0x00000004 size=0x4 source=manual\n"
                                          "0x00000008 bare\n");
    EXPECT_NE(failure.find("line 3"), std::string::npos) << failure;
}

TEST(ParseRecordText, RefusesAFieldWithoutAKeyOrAValue) {
    EXPECT_FALSE(ParseRecordText("format version=1\n0x4 =value\n").Ok());
    EXPECT_FALSE(ParseRecordText("format version=1\n0x4 key=\n").Ok());
}

TEST(ParseRecordText, RefusesTheSameKeyTwiceOnOneRecord) {
    EXPECT_NE(FailureOf("format version=1\n0x4 size=0x4 size=0x8\n").find("set twice"),
              std::string::npos);
}

TEST(ParseRecordText, KeepsSeparatorsInsideAValue) {
    const RecordFile file = ParseOrFail("format version=1\n0x4 name=operator==\n");
    EXPECT_EQ(*file.records.front().Find("name"), "operator==");
}

TEST(RequiredField, ComplainsAboutTheMissingKeyAndTheLine) {
    const RecordFile file = ParseOrFail("format version=1\n0x4 size=0x4\n");
    const core::Result<std::string_view> missing = RequiredField(file.records.front(), "name");
    ASSERT_FALSE(missing.Ok());
    EXPECT_NE(missing.Failure().Message().find("line 2"), std::string::npos);
    EXPECT_NE(missing.Failure().Message().find("name"), std::string::npos);
}

TEST(RequiredAddressField, ReadsAndRefuses) {
    const RecordFile file = ParseOrFail("format version=1\n0x4 size=0x40 target=nowhere\n");
    const Record& record = file.records.front();
    EXPECT_EQ(RequiredAddressField(record, "size").ValueOr(0), 0x40U);
    EXPECT_FALSE(RequiredAddressField(record, "target").Ok());
    EXPECT_FALSE(RequiredAddressField(record, "size2").Ok());
}

TEST(RenderAddress, IsEightUpperCaseDigitsBehindAPrefix) {
    EXPECT_EQ(RenderAddress(0), "0x00000000");
    EXPECT_EQ(RenderAddress(0x12A4B0), "0x0012A4B0");
    EXPECT_EQ(RenderAddress(0xFFFFFFFF), "0xFFFFFFFF");
}

TEST(RenderRecord, JoinsTheAddressAndTheFields) {
    const std::vector<RecordField> fields = {{.key = "size", .value = "0x00000004"},
                                             {.key = "source", .value = "manual"}};
    EXPECT_EQ(RenderRecord(0x100000, fields), "0x00100000 size=0x00000004 source=manual\n");
    EXPECT_EQ(RenderRecord(0x100000, {}), "0x00100000\n");
}

TEST(RenderRecord, ProducesSomethingTheParserReadsBack) {
    const std::vector<RecordField> fields = {{.key = "name", .value = "FieldMain"}};
    const std::string text = RenderVersionLine() + RenderRecord(0x100000, fields);
    const RecordFile file = ParseOrFail(text);
    ASSERT_EQ(file.records.size(), 1U);
    EXPECT_EQ(file.records.front().address, 0x00100000U);
    EXPECT_EQ(*file.records.front().Find("name"), "FieldMain");
}

TEST(RenderComment, MarksEveryLineAndLeavesNoTrailingSpace) {
    EXPECT_EQ(RenderComment("one"), "# one\n");
    EXPECT_EQ(RenderComment("one\ntwo"), "# one\n# two\n");
    EXPECT_EQ(RenderComment("one\n\ntwo\n"), "# one\n#\n# two\n");
    EXPECT_EQ(RenderComment(""), "#\n");
}

}  // namespace
}  // namespace hexlith::domain::analysis
