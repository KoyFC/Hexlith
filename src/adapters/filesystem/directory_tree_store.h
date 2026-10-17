#ifndef HEXLITH_ADAPTERS_FILESYSTEM_DIRECTORY_TREE_STORE_H
#define HEXLITH_ADAPTERS_FILESYSTEM_DIRECTORY_TREE_STORE_H

#include "ports/tree_store.h"

#include <filesystem>

namespace hexlith::adapters::filesystem {

// Keeps each tree as a directory under one root: the description in
// format.txt and the tree itself under files/, empty directories included. A
// directory under the root is a tree when it holds format.txt, and the walk
// for names does not look inside one.
//
// A tree is replaced by writing it whole beside the old one, under a name
// ending in .partial, then moving the old one aside to .old and the new one
// into place. A run that stops while writing leaves the old tree where it
// was. One that stops between the two moves leaves it under .old, which is
// never read as a tree, and the next write clears both.
class DirectoryTreeStore : public ports::TreeStore {
public:
    explicit DirectoryTreeStore(std::filesystem::path root) : root_(std::move(root)) {}

    core::Result<std::vector<std::string>> Names() override;

    core::Result<ports::StoredTree> Read(std::string_view name) override;

    core::Status Replace(std::string_view name, const ports::StoredTree& tree) override;

    [[nodiscard]] std::string Describe() const override;

private:
    [[nodiscard]] core::Result<std::filesystem::path> Resolve(std::string_view name) const;

    std::filesystem::path root_;
};

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_DIRECTORY_TREE_STORE_H
