#include "application/save_storage.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

namespace services = domain::services;

// Trees kept in a map, which is all the use case needs of a store.
class MemoryTreeStore final : public ports::TreeStore {
public:
    core::Result<std::vector<std::string>> Names() override {
        std::vector<std::string> names;
        names.reserve(trees.size());
        for (const auto& [name, tree] : trees) {
            names.push_back(name);
        }
        return names;
    }

    core::Result<ports::StoredTree> Read(std::string_view name) override {
        const auto found = trees.find(std::string{name});
        if (found == trees.end()) {
            return core::Error{"no such tree"};
        }
        return found->second;
    }

    core::Status Replace(std::string_view name, const ports::StoredTree& tree) override {
        trees[std::string{name}] = tree;
        return {};
    }

    [[nodiscard]] std::string Describe() const override {
        return "memory";
    }

    std::map<std::string, ports::StoredTree> trees;
};

constexpr services::SaveDataFormat SaveFormat{
    .total_size = 0x40000, .directories = 1, .files = 2, .duplicate_data = true};
constexpr services::ExtraDataId ExtraId{.media_type = 1, .low = 0x1234, .high = 0};

TEST(StoreSaves, WritesNothingForSaveDataNeverFormatted) {
    MemoryTreeStore store;
    const services::SaveData save;
    const services::ExtraData extra;
    ASSERT_TRUE(StoreSaves(store, save, extra).Ok());
    EXPECT_TRUE(store.trees.empty());
}

TEST(StoreSaves, NamesEachArchiveAndDescribesItsFormat) {
    MemoryTreeStore store;
    services::SaveData save;
    save.Reformat(SaveFormat);
    ASSERT_EQ(save.CreateFile("main", 4), domain::kernel::result::Success);
    services::ExtraData extra;
    extra[ExtraId] = std::make_shared<services::SaveData>(services::ArchiveKind::ExtraData);

    ASSERT_TRUE(StoreSaves(store, save, extra).Ok());
    ASSERT_EQ(store.trees.size(), 2U);
    ASSERT_TRUE(store.trees.contains("savedata"));
    EXPECT_TRUE(store.trees.contains("extdata/00000001-00000000-00001234"));
    const ports::StoredTree& tree = store.trees.at("savedata");
    EXPECT_NE(tree.description.find("total_size = 0x00040000"), std::string::npos)
        << tree.description;
    EXPECT_EQ(tree.files.at("main").size(), 4U);
}

TEST(RestoreSaves, PutsBackWhatWasStored) {
    MemoryTreeStore store;
    services::SaveData save;
    save.Reformat(SaveFormat);
    ASSERT_EQ(save.CreateDirectory("dir"), domain::kernel::result::Success);
    ASSERT_EQ(save.CreateFile("dir/file", 3), domain::kernel::result::Success);
    services::ExtraData extra;
    auto archive = std::make_shared<services::SaveData>(services::ArchiveKind::ExtraData);
    archive->Reformat(services::SaveDataFormat{.total_size = SaveFormat.total_size});
    ASSERT_EQ(archive->CreateFile("fixed", 2), domain::kernel::result::Success);
    extra[ExtraId] = archive;
    ASSERT_TRUE(StoreSaves(store, save, extra).Ok());

    services::SaveData restored;
    services::ExtraData restored_extra;
    const core::Status status = RestoreSaves(store, restored, restored_extra);
    ASSERT_TRUE(status.Ok()) << status.Failure().Message();
    EXPECT_EQ(restored.Format(), SaveFormat);
    EXPECT_EQ(restored.Directories(), save.Directories());
    EXPECT_EQ(restored.Files(), save.Files());

    ASSERT_EQ(restored_extra.size(), 1U);
    const auto& [id, data] = *restored_extra.begin();
    EXPECT_EQ(id, ExtraId);
    EXPECT_EQ(data->Kind(), services::ArchiveKind::ExtraData)
        << "extra data keeps its fixed-size files after a restore";
    EXPECT_EQ(data->SizeOf("fixed"), 2U);
}

TEST(RestoreSaves, RefusesATreeItDidNotWrite) {
    MemoryTreeStore store;
    store.trees["something/else"] = ports::StoredTree{};
    services::SaveData save;
    services::ExtraData extra;
    EXPECT_FALSE(RestoreSaves(store, save, extra).Ok());
}

TEST(RestoreSaves, RefusesAFormatThatIsNotComplete) {
    MemoryTreeStore store;
    ports::StoredTree incomplete;
    incomplete.description = "total_size = 0x10\n";
    store.trees["savedata"] = incomplete;
    services::SaveData save;
    services::ExtraData extra;
    EXPECT_FALSE(RestoreSaves(store, save, extra).Ok());
}

TEST(ExtraDataTreeName, RoundTrips) {
    const std::string name = ExtraDataTreeName(ExtraId);
    EXPECT_EQ(ExtraDataIdFromTreeName(name), ExtraId);
    EXPECT_FALSE(ExtraDataIdFromTreeName("extdata/1-2-3").has_value());
    EXPECT_FALSE(ExtraDataIdFromTreeName("savedata").has_value());
}

}  // namespace
}  // namespace hexlith::application
