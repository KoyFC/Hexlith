#include "application/command_line.h"
#include "application/workspace.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::application {
namespace {

core::Result<ExtractOptions> Parse(std::vector<std::string_view> arguments) {
    return ParseExtractCommandLine(arguments);
}

ExtractOptions ParseOrFail(std::vector<std::string_view> arguments) {
    const core::Result<ExtractOptions> options = ParseExtractCommandLine(arguments);
    EXPECT_TRUE(options.Ok()) << options.Failure().Message();
    return options.Ok() ? options.Value() : ExtractOptions{};
}

TEST(ParseExtractCommandLine, AcceptsNoArgumentsAtAll) {
    const ExtractOptions options = ParseOrFail({});

    EXPECT_FALSE(options.rom_directory.has_value());
    EXPECT_FALSE(options.work_directory.has_value());
    EXPECT_FALSE(options.image.has_value());
    EXPECT_FALSE(options.allow_mismatch);
    EXPECT_FALSE(options.help);
}

TEST(ParseExtractCommandLine, ReadsTheDirectories) {
    const ExtractOptions options =
        ParseOrFail({"--rom-dir", "/srv/rom", "--work-dir", "/srv/work"});

    EXPECT_EQ(options.rom_directory, "/srv/rom");
    EXPECT_EQ(options.work_directory, "/srv/work");
}

TEST(ParseExtractCommandLine, ReadsANamedImage) {
    const ExtractOptions options = ParseOrFail({"--image", "/srv/rom/other.3ds"});
    EXPECT_EQ(options.image, "/srv/rom/other.3ds");
}

TEST(ParseExtractCommandLine, ReadsTheFlags) {
    EXPECT_TRUE(ParseOrFail({"--allow-mismatch"}).allow_mismatch);
    EXPECT_TRUE(ParseOrFail({"--help"}).help);
    EXPECT_TRUE(ParseOrFail({"-h"}).help);
}

TEST(ParseExtractCommandLine, TakesTheOptionsInAnyOrder) {
    const ExtractOptions options =
        ParseOrFail({"--allow-mismatch", "--work-dir", "/srv/work", "--rom-dir", "/srv/rom"});

    EXPECT_EQ(options.rom_directory, "/srv/rom");
    EXPECT_EQ(options.work_directory, "/srv/work");
    EXPECT_TRUE(options.allow_mismatch);
}

TEST(ParseExtractCommandLine, RefusesAnOptionWithNoValue) {
    const core::Result<ExtractOptions> options = Parse({"--rom-dir"});
    ASSERT_FALSE(options.Ok());
    EXPECT_NE(options.Failure().Message().find("needs a value"), std::string::npos);
}

// A missing value followed by another option is the mistake this catches:
// without the check the next option would be swallowed as a path.
TEST(ParseExtractCommandLine, RefusesAnOptionWhoseValueIsAnotherOption) {
    const core::Result<ExtractOptions> options = Parse({"--rom-dir", "--work-dir", "/srv/work"});
    ASSERT_FALSE(options.Ok());
    EXPECT_NE(options.Failure().Message().find("followed by --work-dir"), std::string::npos)
        << options.Failure().Message();
}

TEST(ParseExtractCommandLine, RefusesTheSameOptionTwice) {
    const core::Result<ExtractOptions> options = Parse({"--rom-dir", "/one", "--rom-dir", "/two"});
    ASSERT_FALSE(options.Ok());
    EXPECT_NE(options.Failure().Message().find("was given twice"), std::string::npos);
}

TEST(ParseExtractCommandLine, RefusesAnOptionItDoesNotKnow) {
    const core::Result<ExtractOptions> options = Parse({"--extract-everything"});
    ASSERT_FALSE(options.Ok());
    EXPECT_NE(options.Failure().Message().find("not an option this tool knows"), std::string::npos);
}

TEST(ParseExtractCommandLine, RefusesABarePath) {
    const core::Result<ExtractOptions> options = Parse({"/srv/rom/pokemon-x.3ds"});
    ASSERT_FALSE(options.Ok());
    EXPECT_NE(options.Failure().Message().find("is not an option"), std::string::npos)
        << options.Failure().Message();
}

TEST(ExtractUsage, NamesEveryOptionAndBothVariables) {
    const std::string usage = ExtractUsage();

    EXPECT_NE(usage.find("--rom-dir"), std::string::npos);
    EXPECT_NE(usage.find("--work-dir"), std::string::npos);
    EXPECT_NE(usage.find("--image"), std::string::npos);
    EXPECT_NE(usage.find("--allow-mismatch"), std::string::npos);
    EXPECT_NE(usage.find("--help"), std::string::npos);
    EXPECT_NE(usage.find(RomDirectoryVariable), std::string::npos);
    EXPECT_NE(usage.find(WorkDirectoryVariable), std::string::npos);
    EXPECT_NE(usage.find("read-only"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::application
