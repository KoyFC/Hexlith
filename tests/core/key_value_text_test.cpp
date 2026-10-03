#include "core/key_value_text.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::core {
namespace {

std::vector<KeyValue> ParseOrFail(std::string_view text) {
    Result<std::vector<KeyValue>> parsed = ParseKeyValueText(text);
    EXPECT_TRUE(parsed.Ok()) << parsed.Failure().Message();
    return parsed.Ok() ? parsed.Value() : std::vector<KeyValue>{};
}

TEST(ParseKeyValueText, ReadsKeysAndValues) {
    const std::vector<KeyValue> entries = ParseOrFail("rom_dir = /srv/rom\nwork_dir = /srv/work\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "rom_dir");
    EXPECT_EQ(entries[0].value, "/srv/rom");
    EXPECT_EQ(entries[0].line, 1U);
    EXPECT_EQ(entries[1].key, "work_dir");
    EXPECT_EQ(entries[1].value, "/srv/work");
    EXPECT_EQ(entries[1].line, 2U);
}

TEST(ParseKeyValueText, IgnoresBlankLinesAndComments) {
    const std::vector<KeyValue> entries = ParseOrFail("# where the cartridge lives\n"
                                                      "\n"
                                                      "   \n"
                                                      "rom_dir = /srv/rom\n"
                                                      "   # indented comment, with an = in it\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].line, 4U);
}

TEST(ParseKeyValueText, TrimsAroundTheKeyAndValueButNotInside) {
    const std::vector<KeyValue> entries = ParseOrFail("  rom_dir  =  /srv/my rom  \n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "rom_dir");
    EXPECT_EQ(entries[0].value, "/srv/my rom");
}

TEST(ParseKeyValueText, AcceptsAnEmptyValue) {
    const std::vector<KeyValue> entries = ParseOrFail("rom_dir =\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].value, "");
}

TEST(ParseKeyValueText, KeepsLaterSeparators) {
    const std::vector<KeyValue> entries = ParseOrFail("key = a=b=c\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].value, "a=b=c");
}

TEST(ParseKeyValueText, AcceptsCarriageReturns) {
    const std::vector<KeyValue> entries = ParseOrFail("rom_dir = /srv/rom\r\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].value, "/srv/rom");
}

TEST(ParseKeyValueText, AcceptsAFinalLineWithNoNewline) {
    const std::vector<KeyValue> entries = ParseOrFail("rom_dir = /srv/rom");
    ASSERT_EQ(entries.size(), 1U);
}

TEST(ParseKeyValueText, AcceptsEmptyText) {
    EXPECT_TRUE(ParseKeyValueText("").Value().empty());
}

TEST(ParseKeyValueText, RejectsALineWithNoSeparator) {
    const Result<std::vector<KeyValue>> parsed = ParseKeyValueText("rom_dir /srv/rom\n");
    ASSERT_FALSE(parsed.Ok());
    EXPECT_EQ(parsed.Failure().Message(), "line 1: 'rom_dir /srv/rom' is not a key = value line");
}

TEST(ParseKeyValueText, RejectsAMissingKey) {
    const Result<std::vector<KeyValue>> parsed = ParseKeyValueText("\n = /srv/rom\n");
    ASSERT_FALSE(parsed.Ok());
    EXPECT_EQ(parsed.Failure().Message(), "line 2: the key is missing");
}

TEST(ParseKeyValueText, RejectsARepeatedKeyAndNamesBothLines) {
    const Result<std::vector<KeyValue>> parsed =
        ParseKeyValueText("rom_dir = /old\nwork_dir = /srv/work\nrom_dir = /new\n");
    ASSERT_FALSE(parsed.Ok());
    EXPECT_EQ(parsed.Failure().Message(), "line 3: 'rom_dir' was already set on line 1");
}

TEST(Find, LocatesAKeyAndReportsAMissingOne) {
    const std::vector<KeyValue> entries = ParseOrFail("rom_dir = /srv/rom\n");
    ASSERT_NE(Find(entries, "rom_dir"), nullptr);
    EXPECT_EQ(Find(entries, "rom_dir")->value, "/srv/rom");
    EXPECT_EQ(Find(entries, "work_dir"), nullptr);
}

}  // namespace
}  // namespace hexlith::core
