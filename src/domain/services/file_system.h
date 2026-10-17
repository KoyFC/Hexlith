#ifndef HEXLITH_DOMAIN_SERVICES_FILE_SYSTEM_H
#define HEXLITH_DOMAIN_SERVICES_FILE_SYSTEM_H

#include "core/byte_reader.h"
#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

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
// Three kinds of archive matter before the first module loads. The title's
// own RomFS, served from the cartridge image as raw bytes from IVFC level 3,
// which is what the game's file system library parses itself. Its save data,
// which does not exist yet on a first boot and answers that it is not
// formatted, the answer that sends the game to language selection. And the
// console's system archives, the shared font and the word lists, which are not
// on the cartridge and answer that they are not found.
class FileSystem final : public CommandTable<FileSystem> {
public:
    // The RomFS as the game sees it: level 3 of its IVFC tree, from the
    // cartridge image, which outlives the service.
    explicit FileSystem(core::ByteSpan romfs);

    [[nodiscard]] std::string_view Name() const override {
        return "fs:USER";
    }

    static std::span<const Command> Commands();

    // Archive IDs. Source: ArchiveIdCode in Azahar's archive.h; 3dbrew,
    // Filesystem services.
    static constexpr std::uint32_t SelfNcchArchive = 0x00000003;
    static constexpr std::uint32_t SaveDataArchive = 0x00000004;

    // Results. Source: Azahar's file_sys/errors.h, encoded as 3dbrew's Error
    // codes describes.
    static constexpr kernel::ResultCode NotFound{0xC8804478};
    static constexpr kernel::ResultCode NotFormatted{0xC8A04554};
    static constexpr kernel::ResultCode InvalidPath{0xE0E046BE};
    static constexpr kernel::ResultCode InvalidArchiveHandle{0xC8804465};

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

    // Whether an archive can be opened, and with what result.
    [[nodiscard]] kernel::ResultCode CanOpen(std::uint32_t archive) const;
    [[nodiscard]] OpenResult OpenIn(std::uint32_t archive,
                                    std::span<const std::uint8_t> path) const;
    static void AnswerOpen(kernel::IpcRequest& request, const OpenResult& opened);

    core::ByteSpan romfs_;
    bool initialized_ = false;
    std::uint32_t priority_ = 0;

    // Open archives by the 64-bit handle the game holds, which counts up from
    // one as in Azahar's ArchiveManager.
    std::map<std::uint64_t, std::uint32_t> archives_;
    std::uint64_t next_archive_ = 1;
};

// A file opened on fs:USER: a session of its own, answering reads by offset
// into a mapped buffer. Only read-only files exist so far.
class OpenFileSession final : public CommandTable<OpenFileSession> {
public:
    explicit OpenFileSession(core::ByteSpan contents);

    [[nodiscard]] std::string_view Name() const override {
        return "fs:USER file";
    }

    static std::span<const Command> Commands();

private:
    void Read(kernel::IpcRequest& request);
    void GetSize(kernel::IpcRequest& request);
    void Close(kernel::IpcRequest& request);
    void SetPriority(kernel::IpcRequest& request);
    void GetPriority(kernel::IpcRequest& request);

    core::ByteSpan contents_;
    std::uint32_t priority_ = 0;
    bool closed_ = false;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_FILE_SYSTEM_H
