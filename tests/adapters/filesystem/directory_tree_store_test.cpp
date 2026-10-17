#include "adapters/filesystem/directory_tree_store.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <unistd.h>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

// A directory of its own per test, removed afterwards whether the test passed
// or not, and never the workspace.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("hexlith-tree-test-" + std::to_string(::getpid()) +
                                             "-" + std::to_string(++counter));
        fs::remove_all(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    [[nodiscard]] const fs::path& Path() const {
        return path_;
    }

private:
    fs::path path_;
};

ports::StoredTree SampleTree() {
    ports::StoredTree tree;
    tree.description = "size = 4\n";
    tree.directories = {"empty", "full"};
    tree.files = {{"full/inner", {1, 2, 3}}, {"top", {4}}};
    return tree;
}

TEST(DirectoryTreeStore, HasNoTreesBeforeAnythingIsWritten) {
    const TemporaryDirectory directory;
    DirectoryTreeStore store{directory.Path() / "missing"};
    const core::Result<std::vector<std::string>> names = store.Names();
    ASSERT_TRUE(names.Ok()) << names.Failure().Message();
    EXPECT_TRUE(names.Value().empty());
}

TEST(DirectoryTreeStore, ReadsBackWhatItWrote) {
    const TemporaryDirectory directory;
    DirectoryTreeStore store{directory.Path()};
    ASSERT_TRUE(store.Replace("kind/one", SampleTree()).Ok());

    const core::Result<std::vector<std::string>> names = store.Names();
    ASSERT_TRUE(names.Ok());
    EXPECT_EQ(names.Value(), std::vector<std::string>{"kind/one"});

    const core::Result<ports::StoredTree> tree = store.Read("kind/one");
    ASSERT_TRUE(tree.Ok()) << tree.Failure().Message();
    EXPECT_EQ(tree.Value().description, SampleTree().description);
    EXPECT_EQ(tree.Value().directories, SampleTree().directories) << "an empty directory survives";
    EXPECT_EQ(tree.Value().files, SampleTree().files);
}

TEST(DirectoryTreeStore, ReplacesATreeWhole) {
    const TemporaryDirectory directory;
    DirectoryTreeStore store{directory.Path()};
    ASSERT_TRUE(store.Replace("tree", SampleTree()).Ok());

    ports::StoredTree smaller;
    smaller.description = "size = 1\n";
    constexpr std::uint8_t OnlyByte = 0x5A;
    smaller.files = {{"only", {OnlyByte}}};
    ASSERT_TRUE(store.Replace("tree", smaller).Ok());

    const core::Result<ports::StoredTree> tree = store.Read("tree");
    ASSERT_TRUE(tree.Ok());
    EXPECT_TRUE(tree.Value().directories.empty()) << "nothing of the old tree is left";
    EXPECT_EQ(tree.Value().files, smaller.files);
    EXPECT_FALSE(fs::exists(directory.Path() / "tree.partial"));
    EXPECT_FALSE(fs::exists(directory.Path() / "tree.old"));
}

TEST(DirectoryTreeStore, NeverTakesATreeSetAsideForOne) {
    const TemporaryDirectory directory;
    DirectoryTreeStore store{directory.Path()};
    ASSERT_TRUE(store.Replace("tree", SampleTree()).Ok());

    // What a run that stopped between the two renames would leave.
    fs::rename(directory.Path() / "tree", directory.Path() / "tree.old");
    const core::Result<std::vector<std::string>> names = store.Names();
    ASSERT_TRUE(names.Ok());
    EXPECT_TRUE(names.Value().empty());
    EXPECT_FALSE(store.Read("tree.old").Ok());
}

TEST(DirectoryTreeStore, RefusesANameOrPathThatClimbsOut) {
    const TemporaryDirectory directory;
    DirectoryTreeStore store{directory.Path() / "root"};
    EXPECT_FALSE(store.Replace("../outside", SampleTree()).Ok());
    EXPECT_FALSE(store.Replace("/absolute", SampleTree()).Ok());

    ports::StoredTree escaping;
    escaping.files = {{"../escape", {1}}};
    EXPECT_FALSE(store.Replace("tree", escaping).Ok());
    EXPECT_FALSE(fs::exists(directory.Path() / "root" / "escape"));
}

}  // namespace
}  // namespace hexlith::adapters::filesystem
