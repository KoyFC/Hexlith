#include "adapters/filesystem/scripted_input.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include <unistd.h>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;
namespace button = domain::services::button;

fs::path WriteScript(std::string_view name, std::string_view text) {
    const fs::path path =
        fs::temp_directory_path() /
        ("hexlith-input-test-" + std::to_string(::getpid()) + "-" + std::string{name});
    std::ofstream{path} << text;
    return path;
}

TEST(ScriptedInput, AnswersFromTheScriptItRead) {
    const fs::path path = WriteScript("good", "3 tap b\n");
    core::Result<std::unique_ptr<ScriptedInput>> input = ScriptedInput::Load(path);
    fs::remove(path);
    ASSERT_TRUE(input.Ok()) << input.Failure().Message();
    EXPECT_EQ(input.Value()->StateAt(3).buttons, button::B);
    EXPECT_EQ(input.Value()->Describe(), path.string());
}

TEST(ScriptedInput, NamesTheFileItCouldNotRead) {
    const fs::path path = WriteScript("bad", "3 tap q\n");
    const core::Result<std::unique_ptr<ScriptedInput>> input = ScriptedInput::Load(path);
    fs::remove(path);
    ASSERT_FALSE(input.Ok());
    EXPECT_NE(input.Failure().Message().find(path.string()), std::string::npos);
}

TEST(ScriptedInput, RefusesAFileThatIsNotThere) {
    EXPECT_FALSE(ScriptedInput::Load("/nonexistent/hexlith/input.txt").Ok());
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
