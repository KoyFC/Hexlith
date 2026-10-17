#include "adapters/filesystem/directory_tree_store.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

namespace hexlith::adapters::filesystem {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view DescriptionFile = "format.txt";
constexpr std::string_view FilesDirectory = "files";
constexpr std::string_view PartialSuffix = ".partial";
constexpr std::string_view OldSuffix = ".old";

bool IsSetAside(const fs::path& directory) {
    const std::string name = directory.filename().string();
    return name.ends_with(PartialSuffix) || name.ends_with(OldSuffix);
}

// A relative path with no component that could climb out of where it is put.
// Names come from this program and paths from the archive, which normalises
// them, but this is the last place before the disk.
core::Status CheckRelative(std::string_view path) {
    const fs::path relative{path};
    if (path.empty() || relative.is_absolute()) {
        return core::Error{std::format("'{}' is not a relative path", path)};
    }
    for (const fs::path& part : relative) {
        if (part == ".." || part == "." || part.empty()) {
            return core::Error{std::format("'{}' holds '{}'", path, part.string())};
        }
    }
    return {};
}

core::Status WriteFile(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code failure;
    fs::create_directories(path.parent_path(), failure);
    if (failure) {
        return core::Error{std::format(
            "'{}' could not be created: {}", path.parent_path().string(), failure.message())};
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return core::Error{std::format("'{}' could not be written", path.string())};
    }
    return {};
}

core::Result<std::vector<std::uint8_t>> ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return core::Error{std::format("'{}' could not be opened", path.string())};
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file}, {}};
}

core::Status WriteTree(const fs::path& at, const ports::StoredTree& tree) {
    const std::string_view text = tree.description;
    core::Status described =
        WriteFile(at / DescriptionFile,
                  std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                                text.size()});
    if (!described) {
        return described;
    }
    const fs::path files = at / FilesDirectory;
    std::error_code failure;
    fs::create_directories(files, failure);
    if (failure) {
        return core::Error{
            std::format("'{}' could not be created: {}", files.string(), failure.message())};
    }
    for (const std::string& directory : tree.directories) {
        core::Status checked = CheckRelative(directory);
        if (!checked) {
            return checked;
        }
        fs::create_directories(files / directory, failure);
        if (failure) {
            return core::Error{std::format(
                "'{}' could not be created: {}", (files / directory).string(), failure.message())};
        }
    }
    for (const auto& [path, contents] : tree.files) {
        core::Status checked = CheckRelative(path);
        if (!checked) {
            return checked;
        }
        core::Status written = WriteFile(files / path, contents);
        if (!written) {
            return written;
        }
    }
    return {};
}

}  // namespace

core::Result<fs::path> DirectoryTreeStore::Resolve(std::string_view name) const {
    const core::Status checked = CheckRelative(name);
    if (!checked) {
        return checked.Failure();
    }
    if (IsSetAside(fs::path{name})) {
        return core::Error{std::format("'{}' ends the way a tree set aside does", name)};
    }
    return root_ / name;
}

core::Result<std::vector<std::string>> DirectoryTreeStore::Names() {
    std::vector<std::string> names;
    std::error_code failure;
    if (!fs::exists(root_, failure)) {
        return names;
    }
    fs::recursive_directory_iterator walk{root_, failure};
    if (failure) {
        return core::Error{
            std::format("'{}' could not be read: {}", root_.string(), failure.message())};
    }
    for (; walk != fs::recursive_directory_iterator{}; walk.increment(failure)) {
        if (failure) {
            return core::Error{
                std::format("'{}' could not be read: {}", root_.string(), failure.message())};
        }
        if (!walk->is_directory()) {
            continue;
        }
        if (IsSetAside(walk->path())) {
            walk.disable_recursion_pending();
            continue;
        }
        if (fs::exists(walk->path() / DescriptionFile)) {
            names.push_back(walk->path().lexically_relative(root_).generic_string());
            walk.disable_recursion_pending();
        }
    }
    std::ranges::sort(names);
    return names;
}

core::Result<ports::StoredTree> DirectoryTreeStore::Read(std::string_view name) {
    const core::Result<fs::path> at = Resolve(name);
    if (!at) {
        return at.Failure();
    }
    ports::StoredTree tree;
    const core::Result<std::vector<std::uint8_t>> description =
        ReadFile(at.Value() / DescriptionFile);
    if (!description) {
        return description.Failure();
    }
    tree.description.assign(description.Value().begin(), description.Value().end());

    const fs::path files = at.Value() / FilesDirectory;
    std::error_code failure;
    if (!fs::is_directory(files, failure)) {
        return tree;
    }
    for (fs::recursive_directory_iterator walk{files, failure};
         walk != fs::recursive_directory_iterator{};
         walk.increment(failure)) {
        if (failure) {
            return core::Error{
                std::format("'{}' could not be read: {}", files.string(), failure.message())};
        }
        const std::string relative = walk->path().lexically_relative(files).generic_string();
        if (walk->is_directory()) {
            tree.directories.push_back(relative);
            continue;
        }
        core::Result<std::vector<std::uint8_t>> contents = ReadFile(walk->path());
        if (!contents) {
            return contents.Failure();
        }
        tree.files.emplace(relative, std::move(contents).Value());
    }
    std::ranges::sort(tree.directories);
    return tree;
}

core::Status DirectoryTreeStore::Replace(std::string_view name, const ports::StoredTree& tree) {
    const core::Result<fs::path> at = Resolve(name);
    if (!at) {
        return at.Failure();
    }
    fs::path partial = at.Value();
    partial += PartialSuffix;
    fs::path old = at.Value();
    old += OldSuffix;

    std::error_code failure;
    fs::remove_all(partial, failure);
    fs::remove_all(old, failure);
    core::Status written = WriteTree(partial, tree);
    if (!written) {
        return written;
    }
    if (fs::exists(at.Value(), failure)) {
        fs::rename(at.Value(), old, failure);
        if (failure) {
            return core::Error{std::format(
                "'{}' could not be set aside: {}", at.Value().string(), failure.message())};
        }
    }
    fs::rename(partial, at.Value(), failure);
    if (failure) {
        return core::Error{std::format(
            "'{}' could not be put in place: {}", at.Value().string(), failure.message())};
    }
    fs::remove_all(old, failure);
    return {};
}

std::string DirectoryTreeStore::Describe() const {
    return root_.string();
}

}  // namespace hexlith::adapters::filesystem
