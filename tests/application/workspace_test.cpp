#include "application/workspace.h"

#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

namespace hexlith::application {
namespace {

// The paths are deliberately not the ones on the development machine. A test
// that happens to name the real workspace would pass for the wrong reason.
constexpr std::string_view ArgumentRom = "/tmp/from-argument/rom";
constexpr std::string_view VariableRom = "/tmp/from-variable/rom";
constexpr std::string_view ConfiguredRom = "/tmp/from-config/rom";

WorkspaceInputs WithEverything() {
    WorkspaceInputs inputs;
    inputs.rom_directory_argument = std::string{ArgumentRom};
    inputs.work_directory_argument = "/tmp/from-argument/work";
    inputs.rom_directory_variable = std::string{VariableRom};
    inputs.work_directory_variable = "/tmp/from-variable/work";
    inputs.configuration_text =
        std::format("rom_dir = {}\nwork_dir = /tmp/from-config/work\n", ConfiguredRom);
    inputs.configuration_path = "/tmp/config/hexlith/workspace.conf";
    return inputs;
}

TEST(ResolveWorkspace, PrefersTheCommandLine) {
    const core::Result<Workspace> resolved = ResolveWorkspace(WithEverything());
    ASSERT_TRUE(resolved.Ok()) << resolved.Failure().Message();
    EXPECT_EQ(resolved.Value().rom_directory.path, ArgumentRom);
    EXPECT_EQ(resolved.Value().rom_directory.source, PathSource::CommandLine);
    EXPECT_EQ(resolved.Value().work_directory.source, PathSource::CommandLine);
}

TEST(ResolveWorkspace, FallsBackToTheEnvironment) {
    WorkspaceInputs inputs = WithEverything();
    inputs.rom_directory_argument.reset();
    inputs.work_directory_argument.reset();

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_TRUE(resolved.Ok()) << resolved.Failure().Message();
    EXPECT_EQ(resolved.Value().rom_directory.path, VariableRom);
    EXPECT_EQ(resolved.Value().rom_directory.source, PathSource::Environment);
}

TEST(ResolveWorkspace, FallsBackToTheConfigurationFile) {
    WorkspaceInputs inputs = WithEverything();
    inputs.rom_directory_argument.reset();
    inputs.work_directory_argument.reset();
    inputs.rom_directory_variable.reset();
    inputs.work_directory_variable.reset();

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_TRUE(resolved.Ok()) << resolved.Failure().Message();
    EXPECT_EQ(resolved.Value().rom_directory.path, ConfiguredRom);
    EXPECT_EQ(resolved.Value().rom_directory.source, PathSource::ConfigurationFile);
    EXPECT_EQ(resolved.Value().work_directory.path, "/tmp/from-config/work");
}

TEST(ResolveWorkspace, ResolvesEachPathIndependently) {
    WorkspaceInputs inputs = WithEverything();
    inputs.rom_directory_argument.reset();
    inputs.rom_directory_variable.reset();
    inputs.work_directory_variable.reset();

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_TRUE(resolved.Ok()) << resolved.Failure().Message();
    EXPECT_EQ(resolved.Value().rom_directory.source, PathSource::ConfigurationFile);
    EXPECT_EQ(resolved.Value().work_directory.source, PathSource::CommandLine);
}

TEST(ResolveWorkspace, NamesEverySourceConsultedWhenNothingProvidesAPath) {
    WorkspaceInputs inputs;
    inputs.configuration_path = "/tmp/config/hexlith/workspace.conf";

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_FALSE(resolved.Ok());

    const std::string message = resolved.Failure().Message();
    EXPECT_NE(message.find("no ROM directory was given"), std::string::npos) << message;
    EXPECT_NE(message.find("--rom-dir"), std::string::npos) << message;
    EXPECT_NE(message.find("HEXLITH_ROM_DIR"), std::string::npos) << message;
    EXPECT_NE(message.find("/tmp/config/hexlith/workspace.conf, which does not exist"),
              std::string::npos)
        << message;
}

TEST(ResolveWorkspace, ReportsAMissingWorkDirectoryWhenTheRomDirectoryIsSet) {
    WorkspaceInputs inputs;
    inputs.rom_directory_argument = std::string{ArgumentRom};

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_FALSE(resolved.Ok());
    EXPECT_NE(resolved.Failure().Message().find("no work directory was given"), std::string::npos);
}

TEST(ResolveWorkspace, RejectsARelativePathFromTheEnvironment) {
    WorkspaceInputs inputs;
    inputs.rom_directory_variable = "recomp-data/rom";
    inputs.work_directory_argument = "/tmp/work";

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_FALSE(resolved.Ok());
    EXPECT_NE(resolved.Failure().Message().find("which is relative"), std::string::npos);
}

TEST(ResolveWorkspace, AcceptsARelativePathFromTheCommandLine) {
    WorkspaceInputs inputs;
    inputs.rom_directory_argument = "../rom";
    inputs.work_directory_argument = "work";

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_TRUE(resolved.Ok()) << resolved.Failure().Message();
    EXPECT_EQ(resolved.Value().rom_directory.path, "../rom");
}

TEST(ResolveWorkspace, RejectsAnUnexpandedTilde) {
    WorkspaceInputs inputs;
    inputs.configuration_text = "rom_dir = ~/recomp-data/rom\nwork_dir = /tmp/work\n";
    inputs.configuration_path = "/tmp/config/hexlith/workspace.conf";

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_FALSE(resolved.Ok());
    EXPECT_NE(resolved.Failure().Message().find("only a shell expands"), std::string::npos)
        << resolved.Failure().Message();
}

TEST(ResolveWorkspace, RejectsAnEmptyValue) {
    WorkspaceInputs inputs;
    inputs.rom_directory_argument = "";
    inputs.work_directory_argument = "/tmp/work";

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_FALSE(resolved.Ok());
    EXPECT_NE(resolved.Failure().Message().find("is empty"), std::string::npos);
}

TEST(ResolveWorkspace, ReportsAMalformedConfigurationFileWithItsPath) {
    WorkspaceInputs inputs;
    inputs.configuration_text = "rom_dir /srv/rom\n";
    inputs.configuration_path = "/tmp/config/hexlith/workspace.conf";

    const core::Result<Workspace> resolved = ResolveWorkspace(inputs);
    ASSERT_FALSE(resolved.Ok());
    EXPECT_EQ(resolved.Failure().Message(),
              "/tmp/config/hexlith/workspace.conf: line 1: 'rom_dir /srv/rom' is not a "
              "key = value line");
}

TEST(ConfigurationFilePath, PrefersXdgConfigHome) {
    const std::optional<std::filesystem::path> path =
        ConfigurationFilePath("/tmp/xdg", "/home/someone");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path.value_or(std::filesystem::path{}), "/tmp/xdg/hexlith/workspace.conf");
}

TEST(ConfigurationFilePath, FallsBackToTheHomeDirectory) {
    const std::optional<std::filesystem::path> path =
        ConfigurationFilePath(std::nullopt, "/home/someone");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path.value_or(std::filesystem::path{}),
              "/home/someone/.config/hexlith/workspace.conf");
}

TEST(ConfigurationFilePath, TreatsAnEmptyVariableAsUnset) {
    const std::optional<std::filesystem::path> path = ConfigurationFilePath("", "/home/someone");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path.value_or(std::filesystem::path{}),
              "/home/someone/.config/hexlith/workspace.conf");
}

TEST(ConfigurationFilePath, ReportsNothingWhenNeitherIsSet) {
    EXPECT_FALSE(ConfigurationFilePath(std::nullopt, std::nullopt).has_value());
}

TEST(Describe, NamesEverySource) {
    EXPECT_EQ(Describe(PathSource::CommandLine), "the command line");
    EXPECT_EQ(Describe(PathSource::Environment), "the environment");
    EXPECT_EQ(Describe(PathSource::ConfigurationFile), "the configuration file");
}

}  // namespace
}  // namespace hexlith::application
