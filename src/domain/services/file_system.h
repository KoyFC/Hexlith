#ifndef HEXLITH_DOMAIN_SERVICES_FILE_SYSTEM_H
#define HEXLITH_DOMAIN_SERVICES_FILE_SYSTEM_H

#include "core/byte_reader.h"
#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"
#include "domain/services/save_data.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// fs:USER, the file system as an application sees it: archives opened by ID
// and path, and files in them, each file a session of its own. Commands:
// 3dbrew, Filesystem services; answers: FS_USER and File in Azahar,
// src/core/hle/service/fs/. docs/ipc.md has what is served.
//
// Three kinds of archive matter on the way to the title screen. The title's
// own RomFS, served from the cartridge image as raw bytes from IVFC level 3,
// which is what the game's file system library parses itself. Its save data,
// which on a first boot is not formatted, the answer that sends the game to
// language selection, and which the game then formats and fills. And the
// console's system archives, the shared font and the word lists, which are not
// on the cartridge and answer that they are not found.
class FileSystem final : public CommandTable<FileSystem> {
public:
    // The RomFS as the game sees it: level 3 of its IVFC tree, from the
    // cartridge image, which outlives the service. The save data is shared
    // with whoever writes it out.
    FileSystem(core::ByteSpan romfs,
               std::shared_ptr<SaveData> save,
               std::shared_ptr<ExtraData> extra);

    [[nodiscard]] std::string_view Name() const override {
        return "fs:USER";
    }

    static std::span<const Command> Commands();

    // Archive IDs. Source: ArchiveIdCode in Azahar's archive.h; 3dbrew,
    // Filesystem services.
    static constexpr std::uint32_t SelfNcchArchive = 0x00000003;
    static constexpr std::uint32_t SaveDataArchive = 0x00000004;
    static constexpr std::uint32_t ExtraDataArchive = 0x00000006;

    // Results. Source: Azahar's file_sys/errors.h, encoded as 3dbrew's Error
    // codes describes.
    static constexpr kernel::ResultCode NotFound{0xC8804478};
    static constexpr kernel::ResultCode ExtraDataNotFound{0xC8A04478};
    static constexpr kernel::ResultCode NotFormatted{0xC8A04554};
    static constexpr kernel::ResultCode InvalidPath{0xE0E046BE};
    static constexpr kernel::ResultCode InvalidArchiveHandle{0xC8804465};
    static constexpr kernel::ResultCode Unsupported{0xE0C046F8};

    // ControlArchive's action that makes save data durable. Source: 3dbrew,
    // FS:ControlArchive.
    static constexpr std::uint32_t CommitSaveData = 0;

private:
    struct OpenResult {
        kernel::ResultCode result;
        std::shared_ptr<kernel::SessionHandler> file;
    };

    void Initialize(kernel::IpcRequest& request);
    void InitializeWithSdkVersion(kernel::IpcRequest& request);
    void SetPriority(kernel::IpcRequest& request);
    void GetPriority(kernel::IpcRequest& request);
    void OpenFile(kernel::IpcRequest& request);
    void OpenFileDirectly(kernel::IpcRequest& request);
    void OpenArchive(kernel::IpcRequest& request);
    void CloseArchive(kernel::IpcRequest& request);
    void GetFormatInfo(kernel::IpcRequest& request);
    void FormatSaveData(kernel::IpcRequest& request);
    void FormatThisUserSaveData(kernel::IpcRequest& request);
    void CreateFile(kernel::IpcRequest& request);
    void DeleteFile(kernel::IpcRequest& request);
    void CreateDirectory(kernel::IpcRequest& request);
    void ControlArchive(kernel::IpcRequest& request);
    void CreateExtSaveData(kernel::IpcRequest& request);
    void OpenDirectory(kernel::IpcRequest& request);

    // An open archive: which one, and the data behind it when it is one the
    // game can write.
    struct Mounted {
        std::uint32_t archive = 0;
        std::shared_ptr<SaveData> data;
    };

    struct MountResult {
        kernel::ResultCode result;
        Mounted mounted;
    };

    [[nodiscard]] MountResult Mount(std::uint32_t archive,
                                    std::span<const std::uint8_t> path) const;

    // The save data path a request names inside an open archive, or the
    // result that says why it does not name one.
    struct SavePath {
        kernel::ResultCode result;
        std::shared_ptr<SaveData> data;
        std::string path;
    };

    [[nodiscard]] SavePath
    InSaveData(std::uint64_t handle, std::uint32_t type, std::span<const std::uint8_t> bytes) const;

    [[nodiscard]] OpenResult OpenIn(const Mounted& archive,
                                    std::uint32_t path_type,
                                    std::span<const std::uint8_t> path,
                                    OpenMode mode) const;
    static void AnswerOpen(kernel::IpcRequest& request, const OpenResult& opened);

    core::ByteSpan romfs_;
    std::shared_ptr<SaveData> save_;
    std::shared_ptr<ExtraData> extra_;
    bool initialized_ = false;
    std::uint32_t priority_ = 0;

    // Open archives by the 64-bit handle the game holds, which counts up from
    // one as in Azahar's ArchiveManager.
    std::map<std::uint64_t, Mounted> archives_;
    std::uint64_t next_archive_ = 1;
};

// A file opened on fs:USER: a session of its own, answering reads by offset
// into a mapped buffer. This one is read-only, a file of the RomFS.
class OpenFileSession final : public CommandTable<OpenFileSession> {
public:
    explicit OpenFileSession(core::ByteSpan contents);

    [[nodiscard]] std::string_view Name() const override {
        return "fs:file";
    }

    static std::span<const Command> Commands();

private:
    void Read(kernel::IpcRequest& request);
    void GetSize(kernel::IpcRequest& request);
    void Close(kernel::IpcRequest& request);
    void SetPriority(kernel::IpcRequest& request);
    void GetPriority(kernel::IpcRequest& request);
    void OpenLinkFile(kernel::IpcRequest& request);

    core::ByteSpan contents_;
    std::uint32_t priority_ = 0;
    bool closed_ = false;
};

// A directory of save data or extra data, listed when it was opened. Source:
// Directory in Azahar, src/core/hle/service/fs/directory.cpp, and the entry
// layout in src/core/file_sys/directory_backend.h.
class DirectorySession final : public CommandTable<DirectorySession> {
public:
    explicit DirectorySession(std::vector<SaveData::Entry> entries);

    [[nodiscard]] std::string_view Name() const override {
        return "fs:directory";
    }

    static std::span<const Command> Commands();

    // What one entry looks like to the game.
    static constexpr std::size_t EntrySize = 0x228;
    static std::vector<std::uint8_t> Encode(const SaveData::Entry& entry);

private:
    void Read(kernel::IpcRequest& request);
    void Close(kernel::IpcRequest& request);

    std::vector<SaveData::Entry> entries_;
    std::size_t next_ = 0;
};

// A file of the save data, open for reading, writing or both. Source: File in
// Azahar, src/core/hle/service/fs/file.cpp, and DiskFile in
// src/core/file_sys/disk_archive.cpp.
class SaveFileSession final : public CommandTable<SaveFileSession> {
public:
    SaveFileSession(std::shared_ptr<SaveData> save, std::string path, OpenMode mode);

    [[nodiscard]] std::string_view Name() const override {
        return "fs:file";
    }

    static std::span<const Command> Commands();

private:
    void Read(kernel::IpcRequest& request);
    void Write(kernel::IpcRequest& request);
    void GetSize(kernel::IpcRequest& request);
    void SetSize(kernel::IpcRequest& request);
    void Close(kernel::IpcRequest& request);
    void Flush(kernel::IpcRequest& request);
    void SetPriority(kernel::IpcRequest& request);
    void GetPriority(kernel::IpcRequest& request);
    void OpenLinkFile(kernel::IpcRequest& request);

    std::shared_ptr<SaveData> save_;
    std::string path_;
    OpenMode mode_;
    std::uint32_t priority_ = 0;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_FILE_SYSTEM_H
