#include "domain/services/save_data.h"

#include "core/byte_reader.h"

#include <algorithm>

namespace hexlith::domain::services {
namespace {

// Path types. Source: LowPathType in Azahar's archive_backend.h; 3dbrew,
// Filesystem services, Paths.
constexpr std::uint32_t AsciiPath = 3;
constexpr std::uint32_t Utf16Path = 4;

// Open flags. Source: 3dbrew, Filesystem services.
constexpr std::uint32_t ReadFlag = 1U << 0U;
constexpr std::uint32_t WriteFlag = 1U << 1U;
constexpr std::uint32_t CreateFlag = 1U << 2U;

// Characters Azahar refuses in a path, which games do not use. Source:
// PathParser in Azahar's path_parser.cpp.
constexpr std::string_view RefusedCharacters = "<>\\|:\"*?";

// UTF-16 to UTF-8, for the paths games send as UTF-16. A lone surrogate is
// not something a file name can hold, so it makes the path invalid.
constexpr std::uint32_t SurrogateFirst = 0xD800;
constexpr std::uint32_t LowSurrogateFirst = 0xDC00;
constexpr std::uint32_t SurrogateEnd = 0xE000;
constexpr std::uint32_t SupplementaryBase = 0x10000;
constexpr unsigned SurrogateBits = 10;

// Each UTF-8 continuation byte carries six bits of the code point.
constexpr unsigned ContinuationBits = 6;

void AppendUtf8(std::string& text, std::uint32_t code_point) {
    constexpr std::uint32_t OneByteLimit = 0x80;
    constexpr std::uint32_t TwoByteLimit = 0x800;
    constexpr std::uint32_t ThreeByteLimit = 0x10000;
    constexpr std::uint32_t ContinuationMask = 0x3F;
    constexpr std::uint32_t Continuation = 0x80;
    constexpr std::uint32_t TwoByteLead = 0xC0;
    constexpr std::uint32_t ThreeByteLead = 0xE0;
    constexpr std::uint32_t FourByteLead = 0xF0;

    const auto put = [&text](std::uint32_t byte) { text.push_back(static_cast<char>(byte)); };
    if (code_point < OneByteLimit) {
        put(code_point);
    } else if (code_point < TwoByteLimit) {
        put(TwoByteLead | (code_point >> ContinuationBits));
        put(Continuation | (code_point & ContinuationMask));
    } else if (code_point < ThreeByteLimit) {
        put(ThreeByteLead | (code_point >> (2 * ContinuationBits)));
        put(Continuation | ((code_point >> ContinuationBits) & ContinuationMask));
        put(Continuation | (code_point & ContinuationMask));
    } else {
        put(FourByteLead | (code_point >> (3 * ContinuationBits)));
        put(Continuation | ((code_point >> (2 * ContinuationBits)) & ContinuationMask));
        put(Continuation | ((code_point >> ContinuationBits) & ContinuationMask));
        put(Continuation | (code_point & ContinuationMask));
    }
}

// Both text types end in a terminator, which is not part of the name.
std::optional<std::string> TextOf(std::uint32_t type, std::span<const std::uint8_t> bytes) {
    if (type == AsciiPath) {
        if (bytes.empty()) {
            return std::nullopt;
        }
        return std::string(bytes.begin(), bytes.end() - 1);
    }
    if (type != Utf16Path || bytes.size() < 2 || bytes.size() % 2 != 0) {
        return std::nullopt;
    }

    std::string text;
    const std::size_t units = (bytes.size() / 2) - 1;
    for (std::size_t index = 0; index < units; ++index) {
        const std::uint32_t unit = bytes[index * 2] | (std::uint32_t{bytes[(index * 2) + 1]} << 8U);
        if (unit < SurrogateFirst || unit >= SurrogateEnd) {
            AppendUtf8(text, unit);
            continue;
        }
        if (unit >= LowSurrogateFirst || index + 1 >= units) {
            return std::nullopt;
        }
        ++index;
        const std::uint32_t low = bytes[index * 2] | (std::uint32_t{bytes[(index * 2) + 1]} << 8U);
        if (low < LowSurrogateFirst || low >= SurrogateEnd) {
            return std::nullopt;
        }
        AppendUtf8(text,
                   SupplementaryBase + ((unit - SurrogateFirst) << SurrogateBits) +
                       (low - LowSurrogateFirst));
    }
    return text;
}

}  // namespace

OpenMode OpenMode::FromFlags(std::uint32_t flags) {
    return OpenMode{.read = (flags & ReadFlag) != 0,
                    .write = (flags & WriteFlag) != 0,
                    .create = (flags & CreateFlag) != 0};
}

std::optional<std::string> SaveDataPath(std::uint32_t type, std::span<const std::uint8_t> bytes) {
    const std::optional<std::string> text = TextOf(type, bytes);
    if (!text.has_value() || text->empty() || text->front() != '/') {
        return std::nullopt;
    }
    if (text->find_first_of(RefusedCharacters) != std::string::npos) {
        return std::nullopt;
    }

    // Empty components and "." are dropped and ".." climbs, but never above
    // the root.
    std::vector<std::string> components;
    std::size_t start = 1;
    while (start <= text->size()) {
        const std::size_t end = std::min(text->find('/', start), text->size());
        const std::string component = text->substr(start, end - start);
        start = end + 1;
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            if (components.empty()) {
                return std::nullopt;
            }
            components.pop_back();
            continue;
        }
        components.push_back(component);
    }

    std::string path;
    for (const std::string& component : components) {
        if (!path.empty()) {
            path.push_back('/');
        }
        path += component;
    }
    return path;
}

void SaveData::Reformat(const SaveDataFormat& format) {
    format_ = format;
    directories_.clear();
    files_.clear();
}

SaveData::Status SaveData::StatusOf(const std::string& path) const {
    if (path.empty() || directories_.contains(path)) {
        return Status::Directory;
    }
    if (files_.contains(path)) {
        return Status::File;
    }

    // Walk the parents from the root down, the way Azahar's PathParser does,
    // so that a file in the middle of a path is told apart from a directory
    // that is missing.
    std::size_t slash = path.find('/');
    while (slash != std::string::npos) {
        const std::string parent = path.substr(0, slash);
        if (files_.contains(parent)) {
            return Status::FileInPath;
        }
        if (!directories_.contains(parent)) {
            return Status::ParentMissing;
        }
        slash = path.find('/', slash + 1);
    }
    return Status::Absent;
}

kernel::ResultCode SaveData::CreateFile(const std::string& path, std::uint64_t size) {
    if (kind_ == ArchiveKind::ExtraData && size == 0) {
        return save_result::UnsupportedOpenFlags;
    }
    switch (StatusOf(path)) {
    case Status::ParentMissing:
        return save_result::PathNotFound;
    case Status::FileInPath:
        return save_result::UnexpectedFileOrDirectory;
    case Status::File:
    case Status::Directory:
        return save_result::FileAlreadyExists;
    case Status::Absent:
        break;
    }
    files_.emplace(path, Bytes(size, 0));
    return kernel::result::Success;
}

kernel::ResultCode SaveData::DeleteFile(const std::string& path) {
    switch (StatusOf(path)) {
    case Status::ParentMissing:
        return save_result::PathNotFound;
    case Status::File:
        files_.erase(path);
        return kernel::result::Success;
    case Status::FileInPath:
    case Status::Directory:
    case Status::Absent:
        return save_result::FileNotFound;
    }
    return save_result::FileNotFound;
}

kernel::ResultCode SaveData::CreateDirectory(const std::string& path) {
    switch (StatusOf(path)) {
    case Status::ParentMissing:
        return save_result::PathNotFound;
    case Status::FileInPath:
        return save_result::UnexpectedFileOrDirectory;
    case Status::File:
    case Status::Directory:
        return save_result::DirectoryAlreadyExists;
    case Status::Absent:
        break;
    }
    directories_.insert(path);
    return kernel::result::Success;
}

kernel::ResultCode SaveData::Open(const std::string& path, OpenMode mode) {
    if (!mode.read && !mode.write && !mode.create) {
        return save_result::UnsupportedOpenFlags;
    }
    if (mode.create && (!mode.write || kind_ == ArchiveKind::ExtraData)) {
        return save_result::UnsupportedOpenFlags;
    }

    switch (StatusOf(path)) {
    case Status::ParentMissing:
        return save_result::PathNotFound;
    case Status::FileInPath:
    case Status::Directory:
        return save_result::UnexpectedFileOrDirectory;
    case Status::Absent:
        if (!mode.create) {
            return save_result::FileNotFound;
        }
        files_.emplace(path, Bytes{});
        return kernel::result::Success;
    case Status::File:
        return kernel::result::Success;
    }
    return save_result::FileNotFound;
}

SaveData::Listing SaveData::List(const std::string& path) const {
    switch (StatusOf(path)) {
    case Status::ParentMissing:
    case Status::Absent:
        return {.result = save_result::PathNotFound, .entries = {}};
    case Status::FileInPath:
    case Status::File:
        return {.result = save_result::UnexpectedFileOrDirectory, .entries = {}};
    case Status::Directory:
        break;
    }

    // A child is a key with this directory as its prefix and no slash after
    // it. Both containers are sorted, so merging them keeps the order.
    const std::string prefix = path.empty() ? std::string{} : path + "/";
    const auto is_child = [&prefix](const std::string& key) {
        return key.size() > prefix.size() && key.starts_with(prefix) &&
               key.find('/', prefix.size()) == std::string::npos;
    };
    std::vector<Entry> entries;
    for (const std::string& directory : directories_) {
        if (is_child(directory)) {
            entries.push_back(
                {.name = directory.substr(prefix.size()), .is_directory = true, .size = 0});
        }
    }
    for (const auto& [name, contents] : files_) {
        if (is_child(name)) {
            entries.push_back({.name = name.substr(prefix.size()),
                               .is_directory = false,
                               .size = contents.size()});
        }
    }
    std::ranges::sort(entries, {}, &Entry::name);
    return {.result = kernel::result::Success, .entries = std::move(entries)};
}

std::uint64_t SaveData::SizeOf(const std::string& path) const {
    const auto file = files_.find(path);
    return file == files_.end() ? 0 : file->second.size();
}

std::span<const std::uint8_t>
SaveData::Read(const std::string& path, std::uint64_t offset, std::uint32_t length) const {
    const auto file = files_.find(path);
    if (file == files_.end()) {
        return {};
    }
    const std::span<const std::uint8_t> contents = file->second;
    const std::size_t start = std::min<std::uint64_t>(offset, contents.size());
    const std::size_t count = std::min<std::size_t>(length, contents.size() - start);
    return contents.subspan(start, count);
}

SaveData::Written
SaveData::Write(const std::string& path, std::uint64_t offset, std::span<const std::uint8_t> data) {
    Bytes& contents = files_[path];
    if (kind_ == ArchiveKind::ExtraData) {
        if (offset > contents.size()) {
            return {.result = save_result::WriteBeyondEnd, .count = 0};
        }
        data = data.first(std::min<std::size_t>(data.size(), contents.size() - offset));
    } else if (offset + data.size() > contents.size()) {
        contents.resize(offset + data.size(), 0);
    }
    std::ranges::copy(data, contents.begin() + static_cast<std::ptrdiff_t>(offset));
    return {.result = kernel::result::Success, .count = static_cast<std::uint32_t>(data.size())};
}

void SaveData::Resize(const std::string& path, std::uint64_t size) {
    if (kind_ == ArchiveKind::ExtraData) {
        return;
    }
    files_[path].resize(size, 0);
}

void SaveData::Commit() {
    if (on_commit_) {
        on_commit_(*this);
    }
}

void SaveData::Restore(const SaveDataFormat& format,
                       std::set<std::string> directories,
                       std::map<std::string, Bytes> files) {
    format_ = format;
    directories_ = std::move(directories);
    files_ = std::move(files);
}

std::u16string Utf16Of(std::string_view name) {
    constexpr std::uint32_t ContinuationMask = 0x3F;
    constexpr std::uint32_t TwoByteLeadMask = 0x1F;
    constexpr std::uint32_t ThreeByteLeadMask = 0x0F;
    constexpr std::uint32_t FourByteLeadMask = 0x07;
    constexpr std::uint8_t TwoByteLead = 0xC0;
    constexpr std::uint8_t ThreeByteLead = 0xE0;
    constexpr std::uint8_t FourByteLead = 0xF0;

    // The names come from SaveDataPath, so they are well formed.
    std::u16string text;
    std::size_t index = 0;
    while (index < name.size()) {
        const auto lead = static_cast<std::uint8_t>(name[index]);
        std::uint32_t code_point = lead;
        std::size_t continuation = 0;
        if (lead >= FourByteLead) {
            code_point = lead & FourByteLeadMask;
            continuation = 3;
        } else if (lead >= ThreeByteLead) {
            code_point = lead & ThreeByteLeadMask;
            continuation = 2;
        } else if (lead >= TwoByteLead) {
            code_point = lead & TwoByteLeadMask;
            continuation = 1;
        }
        for (std::size_t byte = 1; byte <= continuation && index + byte < name.size(); ++byte) {
            code_point = (code_point << ContinuationBits) |
                         (static_cast<std::uint8_t>(name[index + byte]) & ContinuationMask);
        }
        index += continuation + 1;

        if (code_point < SupplementaryBase) {
            text.push_back(static_cast<char16_t>(code_point));
            continue;
        }
        const std::uint32_t offset = code_point - SupplementaryBase;
        text.push_back(static_cast<char16_t>(SurrogateFirst + (offset >> SurrogateBits)));
        text.push_back(
            static_cast<char16_t>(LowSurrogateFirst + (offset & ((1U << SurrogateBits) - 1))));
    }
    return text;
}

std::optional<ExtraDataId> ExtraDataIdFromPath(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t PathSize = 12;
    if (bytes.size() != PathSize) {
        return std::nullopt;
    }
    return ExtraDataId{.media_type = core::ReadU32At(bytes, 0).ValueOr(0),
                       .low = core::ReadU32At(bytes, 4).ValueOr(0),
                       .high = core::ReadU32At(bytes, 8).ValueOr(0)};
}

}  // namespace hexlith::domain::services
