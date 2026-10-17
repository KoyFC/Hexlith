#ifndef HEXLITH_DOMAIN_SERVICES_SAVE_DATA_H
#define HEXLITH_DOMAIN_SERVICES_SAVE_DATA_H

#include "domain/kernel/result_code.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::services {

// The title's save data archive: a small tree of directories and files that
// the game formats once, on its first boot, and reads on every boot after.
//
// It lives in memory. The console writes save data through a journal and makes
// it durable when the title commits it, and that is where this is written out
// too: the application layer hands a function that runs on every commit, and
// another that runs when the process ends. Until then nothing touches the
// disk, which is what lets a run start from an empty archive and stay exactly
// reproducible. ADR 0015 records the choice.
//
// Extra data, the archive a title keeps on the SD card beside its save data, is
// the same tree with fixed-size files: one is created at its final size, a
// write stops at its end, and it cannot be resized or created by opening it.
// Source: ExtSaveDataArchive in Azahar's archive_extsavedata.cpp.
//
// Paths are kept as their components joined by a slash with no leading one,
// so the root is the empty string. Results are the console's: Azahar's
// src/core/file_sys/savedata_archive.cpp maps each case the same way.

// What FormatSaveData was asked for. Source: ArchiveFormatInfo in Azahar's
// src/core/file_sys/archive_backend.h; 3dbrew, FS:FormatSaveData.
struct SaveDataFormat {
    // In bytes: the request gives blocks of 512.
    std::uint32_t total_size = 0;
    std::uint32_t directories = 0;
    std::uint32_t files = 0;
    bool duplicate_data = false;

    friend bool operator==(const SaveDataFormat&, const SaveDataFormat&) = default;
};

// How a file is opened. Source: 3dbrew, Filesystem services, open flags.
struct OpenMode {
    bool read = false;
    bool write = false;
    bool create = false;

    static OpenMode FromFlags(std::uint32_t flags);
};

// A path as the game sends it, a type and its bytes, turned into the form the
// archive keys on. Source: FileSys::Path and PathParser in Azahar; 3dbrew,
// Filesystem services, Paths. Only text paths name anything in save data;
// another type, a path that does not start at the root, one with a character
// the console's file names cannot hold, or one that climbs above the root,
// has no value.
std::optional<std::string> SaveDataPath(std::uint32_t type, std::span<const std::uint8_t> bytes);

namespace save_result {

// Source: Azahar's src/core/file_sys/errors.h, encoded as 3dbrew's Error
// codes describes.
constexpr kernel::ResultCode FileNotFound{0xC8804470};
constexpr kernel::ResultCode PathNotFound{0xC8804471};
constexpr kernel::ResultCode FileAlreadyExists{0xC82044B4};
constexpr kernel::ResultCode DirectoryAlreadyExists{0xC82044B9};
constexpr kernel::ResultCode InvalidOpenFlags{0xC92044E6};
constexpr kernel::ResultCode DirectoryNotEmpty{0xC92044F0};
constexpr kernel::ResultCode UnsupportedOpenFlags{0xE0C046F8};
constexpr kernel::ResultCode UnexpectedFileOrDirectory{0xE0C04702};
constexpr kernel::ResultCode WriteBeyondEnd{0xE0E046C1};

}  // namespace save_result

enum class ArchiveKind : std::uint8_t { SaveData, ExtraData };

class SaveData {
public:
    using Bytes = std::vector<std::uint8_t>;

    struct Written {
        kernel::ResultCode result;
        std::uint32_t count = 0;
    };

    explicit SaveData(ArchiveKind kind = ArchiveKind::SaveData) : kind_(kind) {}

    [[nodiscard]] ArchiveKind Kind() const {
        return kind_;
    }

    [[nodiscard]] bool IsFormatted() const {
        return format_.has_value();
    }

    [[nodiscard]] const std::optional<SaveDataFormat>& Format() const {
        return format_;
    }

    // Formatting empties the archive, as on the console.
    void Reformat(const SaveDataFormat& format);

    kernel::ResultCode CreateFile(const std::string& path, std::uint64_t size);
    kernel::ResultCode DeleteFile(const std::string& path);
    kernel::ResultCode CreateDirectory(const std::string& path);

    // What a directory holds, in the order its names sort, or the result that
    // says why it cannot be listed.
    struct Entry {
        std::string name;
        bool is_directory = false;
        std::uint64_t size = 0;
    };

    struct Listing {
        kernel::ResultCode result;
        std::vector<Entry> entries;
    };

    [[nodiscard]] Listing List(const std::string& path) const;

    // Whether a file can be opened with this mode, creating it when the mode
    // asks for that and it does not exist.
    kernel::ResultCode Open(const std::string& path, OpenMode mode);

    // Access to a file that is open. The path is known to name a file.
    [[nodiscard]] std::uint64_t SizeOf(const std::string& path) const;
    [[nodiscard]] std::span<const std::uint8_t>
    Read(const std::string& path, std::uint64_t offset, std::uint32_t length) const;
    // In save data a write past the end grows the file, filling the gap with
    // zeros. In extra data it stops at the end.
    Written
    Write(const std::string& path, std::uint64_t offset, std::span<const std::uint8_t> data);
    // Extra data keeps its size and answers success all the same, as Azahar
    // does.
    void Resize(const std::string& path, std::uint64_t size);

    // The title asked for what it has written to be made durable.
    void Commit();

    // What is in the archive, for whoever writes it out or reads it back in.
    [[nodiscard]] const std::map<std::string, Bytes>& Files() const {
        return files_;
    }

    [[nodiscard]] const std::set<std::string>& Directories() const {
        return directories_;
    }

    // Puts back an archive written out earlier.
    void Restore(const SaveDataFormat& format,
                 std::set<std::string> directories,
                 std::map<std::string, Bytes> files);

    // Runs on every commit, with the archive as it then stands.
    void OnCommit(std::function<void(const SaveData&)> handler) {
        on_commit_ = std::move(handler);
    }

private:
    enum class Status : std::uint8_t { ParentMissing, FileInPath, File, Directory, Absent };

    [[nodiscard]] Status StatusOf(const std::string& path) const;

    ArchiveKind kind_;
    std::optional<SaveDataFormat> format_;
    std::set<std::string> directories_;
    std::map<std::string, Bytes> files_;
    std::function<void(const SaveData&)> on_commit_;
};

// Which extra data archive a path names: a media type and a 64-bit ID in two
// halves. Source: ExtSaveDataArchivePath in Azahar's archive_extsavedata.h;
// 3dbrew, Filesystem services, ExtSaveData.
struct ExtraDataId {
    std::uint32_t media_type = 0;
    std::uint32_t low = 0;
    std::uint32_t high = 0;

    friend auto operator<=>(const ExtraDataId&, const ExtraDataId&) = default;
};

// A name as the console's directory entries hold it, UTF-16 from the UTF-8
// the archive keys on.
std::u16string Utf16Of(std::string_view name);

// The binary path OpenArchive names extra data by: three words.
std::optional<ExtraDataId> ExtraDataIdFromPath(std::span<const std::uint8_t> bytes);

// The title's extra data archives, by ID. None exists until the title creates
// it.
using ExtraData = std::map<ExtraDataId, std::shared_ptr<SaveData>>;

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_SAVE_DATA_H
