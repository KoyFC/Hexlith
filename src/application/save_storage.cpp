#include "application/save_storage.h"

#include "core/key_value_text.h"

#include <array>
#include <cctype>
#include <format>
#include <set>

namespace hexlith::application {
namespace {

namespace services = domain::services;

constexpr std::string_view SaveDataName = "savedata";
constexpr std::string_view ExtraDataPrefix = "extdata/";

constexpr std::string_view TotalSizeKey = "total_size";
constexpr std::string_view DirectoriesKey = "directories";
constexpr std::string_view FilesKey = "files";
constexpr std::string_view DuplicateDataKey = "duplicate_data";

constexpr std::uint64_t Hexadecimal = 16;

std::string Describe(const services::SaveDataFormat& format) {
    return std::format("# The format the title gave this archive.\n"
                       "{} = {:#010x}\n{} = {}\n{} = {}\n{} = {}\n",
                       TotalSizeKey,
                       format.total_size,
                       DirectoriesKey,
                       format.directories,
                       FilesKey,
                       format.files,
                       DuplicateDataKey,
                       format.duplicate_data ? 1 : 0);
}

// A word in decimal, or in hexadecimal after 0x, parsed by hand as
// record_text.cpp parses its counts, so that a string_view is read without
// taking its data pointer.
std::optional<std::uint32_t> ParseWord(std::string_view text) {
    constexpr std::uint64_t Decimal = 10;
    constexpr std::uint64_t Limit = 0xFFFFFFFF;
    constexpr std::uint64_t DigitsBeforeLetters = 10;

    std::uint64_t base = Decimal;
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
        base = Hexadecimal;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        std::uint64_t digit = base;
        if (lower >= '0' && lower <= '9') {
            digit = static_cast<std::uint64_t>(lower - '0');
        } else if (lower >= 'a' && lower <= 'f') {
            digit = DigitsBeforeLetters + static_cast<std::uint64_t>(lower - 'a');
        }
        if (digit >= base) {
            return std::nullopt;
        }
        value = (value * base) + digit;
        if (value > Limit) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

core::Result<std::uint32_t> NumberOf(const std::vector<core::KeyValue>& entries,
                                     std::string_view key) {
    const core::KeyValue* entry = core::Find(entries, key);
    if (entry == nullptr) {
        return core::Error{std::format("the format does not give {}", key)};
    }
    const std::optional<std::uint32_t> value = ParseWord(entry->value);
    if (!value.has_value()) {
        return core::Error{
            std::format("line {}: {} is not a number: '{}'", entry->line, key, entry->value)};
    }
    return *value;
}

core::Result<services::SaveDataFormat> FormatFrom(std::string_view description) {
    const core::Result<std::vector<core::KeyValue>> entries = core::ParseKeyValueText(description);
    if (!entries) {
        return entries.Failure();
    }
    services::SaveDataFormat format;
    const std::array<std::pair<std::string_view, std::uint32_t*>, 3> numbers = {{
        {TotalSizeKey, &format.total_size},
        {DirectoriesKey, &format.directories},
        {FilesKey, &format.files},
    }};
    for (const auto& [key, into] : numbers) {
        const core::Result<std::uint32_t> value = NumberOf(entries.Value(), key);
        if (!value) {
            return value.Failure();
        }
        *into = value.Value();
    }
    const core::Result<std::uint32_t> duplicate = NumberOf(entries.Value(), DuplicateDataKey);
    if (!duplicate) {
        return duplicate.Failure();
    }
    format.duplicate_data = duplicate.Value() != 0;
    return format;
}

ports::StoredTree TreeOf(const services::SaveData& archive) {
    ports::StoredTree tree;
    tree.description = Describe(archive.Format().value_or(services::SaveDataFormat{}));
    tree.directories.assign(archive.Directories().begin(), archive.Directories().end());
    tree.files = archive.Files();
    return tree;
}

core::Status Restore(ports::TreeStore& store, std::string_view name, services::SaveData& into) {
    core::Result<ports::StoredTree> tree = store.Read(name);
    if (!tree) {
        return tree.Failure();
    }
    const core::Result<services::SaveDataFormat> format = FormatFrom(tree.Value().description);
    if (!format) {
        return format.Failure().WithContext(std::string{name});
    }
    into.Restore(
        format.Value(),
        std::set<std::string>(tree.Value().directories.begin(), tree.Value().directories.end()),
        std::move(tree.Value().files));
    return {};
}

// Each word of an extra data name is eight hexadecimal digits.
constexpr std::size_t WordDigits = 8;

std::optional<std::uint32_t> HexWord(std::string_view text) {
    if (text.size() != WordDigits) {
        return std::nullopt;
    }
    return ParseWord(std::string{"0x"} + std::string{text});
}

}  // namespace

std::string SaveDataTreeName() {
    return std::string{SaveDataName};
}

std::string ExtraDataTreeName(const services::ExtraDataId& id) {
    return std::format("{}{:08x}-{:08x}-{:08x}", ExtraDataPrefix, id.media_type, id.high, id.low);
}

std::optional<services::ExtraDataId> ExtraDataIdFromTreeName(std::string_view name) {
    // Three words with a dash after each of the first two.
    constexpr std::size_t Stride = WordDigits + 1;
    constexpr std::size_t Length = (3 * Stride) - 1;
    if (!name.starts_with(ExtraDataPrefix)) {
        return std::nullopt;
    }
    name.remove_prefix(ExtraDataPrefix.size());
    if (name.size() != Length || name[WordDigits] != '-' || name[Stride + WordDigits] != '-') {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> media = HexWord(name.substr(0, WordDigits));
    const std::optional<std::uint32_t> high = HexWord(name.substr(Stride, WordDigits));
    const std::optional<std::uint32_t> low = HexWord(name.substr(2 * Stride, WordDigits));
    if (!media.has_value() || !high.has_value() || !low.has_value()) {
        return std::nullopt;
    }
    return services::ExtraDataId{.media_type = *media, .low = *low, .high = *high};
}

core::Status
RestoreSaves(ports::TreeStore& store, services::SaveData& save, services::ExtraData& extra) {
    const core::Result<std::vector<std::string>> names = store.Names();
    if (!names) {
        return names.Failure();
    }
    for (const std::string& name : names.Value()) {
        if (name == SaveDataName) {
            const core::Status restored = Restore(store, name, save);
            if (!restored) {
                return restored;
            }
            continue;
        }
        const std::optional<services::ExtraDataId> id = ExtraDataIdFromTreeName(name);
        if (!id.has_value()) {
            return core::Error{
                std::format("{} holds '{}', which is neither save data nor extra data",
                            store.Describe(),
                            name)};
        }
        auto archive = std::make_shared<services::SaveData>(services::ArchiveKind::ExtraData);
        const core::Status restored = Restore(store, name, *archive);
        if (!restored) {
            return restored;
        }
        extra[*id] = std::move(archive);
    }
    return {};
}

core::Status StoreSaves(ports::TreeStore& store,
                        const services::SaveData& save,
                        const services::ExtraData& extra) {
    if (save.IsFormatted()) {
        const core::Status stored = store.Replace(SaveDataName, TreeOf(save));
        if (!stored) {
            return stored;
        }
    }
    for (const auto& [id, archive] : extra) {
        const core::Status stored = store.Replace(ExtraDataTreeName(id), TreeOf(*archive));
        if (!stored) {
            return stored;
        }
    }
    return {};
}

}  // namespace hexlith::application
