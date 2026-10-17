#ifndef HEXLITH_PORTS_TREE_STORE_H
#define HEXLITH_PORTS_TREE_STORE_H

#include "core/result.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::ports {

// A small tree of directories and files kept from one run to the next, with
// a line of text that describes it. Paths are relative and use forward
// slashes.
struct StoredTree {
    std::string description;
    std::vector<std::string> directories;
    std::map<std::string, std::vector<std::uint8_t>> files;
};

// Where the trees a run keeps are put: save data and extra data, each under a
// name of its own, which may itself hold a slash. Replacing a tree is all or
// nothing, so a run that stops halfway leaves the old tree or the new one and
// never a mixture.
class TreeStore {
public:
    TreeStore() = default;
    virtual ~TreeStore() = default;

    TreeStore(const TreeStore&) = delete;
    TreeStore& operator=(const TreeStore&) = delete;
    TreeStore(TreeStore&&) = delete;
    TreeStore& operator=(TreeStore&&) = delete;

    // The names of the trees stored, in order.
    virtual core::Result<std::vector<std::string>> Names() = 0;

    virtual core::Result<StoredTree> Read(std::string_view name) = 0;

    virtual core::Status Replace(std::string_view name, const StoredTree& tree) = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_TREE_STORE_H
