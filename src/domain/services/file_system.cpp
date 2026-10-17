#include "domain/services/file_system.h"

#include "core/byte_reader.h"

#include <algorithm>
#include <array>

namespace hexlith::domain::services {
namespace {

// GetFormatInfo answers four words after its result.
constexpr std::uint32_t FormatInfoReplyWords = 5;

// A SelfNCCH file path is twelve bytes: a type word, then an ExeFS file name.
// Type 0 is the RomFS. Source: SelfNCCHFilePath in Azahar.
constexpr std::size_t SelfNcchPathSize = 12;
constexpr std::uint32_t SelfNcchRomFs = 0;

std::shared_ptr<kernel::Object> AsObject(const std::shared_ptr<kernel::SessionHandler>& file) {
    if (file == nullptr) {
        return nullptr;
    }
    return std::make_shared<kernel::ClientSession>(file);
}

}  // namespace

FileSystem::FileSystem(core::ByteSpan romfs) : romfs_(romfs) {}

std::span<const FileSystem::Command> FileSystem::Commands() {
    static constexpr std::array<Command, 12> Table = {{
        {.id = 0x0801, .name = "Initialize", .handler = &FileSystem::Initialize},
        {.id = 0x0802, .name = "OpenFile", .handler = &FileSystem::OpenFile},
        {.id = 0x0803, .name = "OpenFileDirectly", .handler = &FileSystem::OpenFileDirectly},
        {.id = 0x0808, .name = "CreateFile"},
        {.id = 0x080B, .name = "OpenDirectory"},
        {.id = 0x080C, .name = "OpenArchive", .handler = &FileSystem::OpenArchive},
        {.id = 0x080E, .name = "CloseArchive", .handler = &FileSystem::CloseArchive},
        {.id = 0x0845, .name = "GetFormatInfo", .handler = &FileSystem::GetFormatInfo},
        {.id = 0x084C, .name = "FormatSaveData"},
        {.id = 0x0861,
         .name = "InitializeWithSdkVersion",
         .handler = &FileSystem::InitializeWithSdkVersion},
        {.id = 0x0862, .name = "SetPriority", .handler = &FileSystem::SetPriority},
        {.id = 0x0863, .name = "GetPriority", .handler = &FileSystem::GetPriority},
    }};
    return Table;
}

void FileSystem::Initialize(kernel::IpcRequest& request) {
    static_cast<void>(request.PopProcessId());
    initialized_ = true;
    request.ReplyWith(kernel::result::Success);
}

void FileSystem::InitializeWithSdkVersion(kernel::IpcRequest& request) {
    // The SDK version the title was built with, which the service only records.
    static_cast<void>(request.Pop());
    static_cast<void>(request.PopProcessId());
    initialized_ = true;
    request.ReplyWith(kernel::result::Success);
}

void FileSystem::SetPriority(kernel::IpcRequest& request) {
    priority_ = request.Pop();
    request.ReplyWith(kernel::result::Success);
}

void FileSystem::GetPriority(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(priority_);
}

kernel::ResultCode FileSystem::CanOpen(std::uint32_t archive) const {
    switch (archive) {
    case SelfNcchArchive:
        return kernel::result::Success;
    case SaveDataArchive:
        // A first boot: the title has never formatted its save data.
        return NotFormatted;
    default:
        // The system archives, extra data and the rest: nothing here holds
        // them, and the console's own system files are not on the cartridge.
        return NotFound;
    }
}

FileSystem::OpenResult FileSystem::OpenIn(std::uint32_t archive,
                                          std::span<const std::uint8_t> path) const {
    if (archive != SelfNcchArchive) {
        return {.result = NotFound, .file = nullptr};
    }
    if (path.size() != SelfNcchPathSize) {
        return {.result = InvalidPath, .file = nullptr};
    }
    const std::uint32_t type = core::ReadU32At(path, 0).ValueOr(~0U);
    if (type != SelfNcchRomFs) {
        return {.result = NotFound, .file = nullptr};
    }
    return {.result = kernel::result::Success, .file = std::make_shared<OpenFileSession>(romfs_)};
}

void FileSystem::AnswerOpen(kernel::IpcRequest& request, const OpenResult& opened) {
    request.Begin(1, 2);
    request.Push(opened.result);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {AsObject(opened.file)};
    request.PushMoveObjects(objects);
}

void FileSystem::OpenFile(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the transaction
    const std::uint64_t handle = request.Pop64();
    static_cast<void>(request.Pop());  // the path's type
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.Pop());  // the open mode
    static_cast<void>(request.Pop());  // attributes
    const std::vector<std::uint8_t> path = request.PopStaticBuffer();

    const auto archive = archives_.find(handle);
    if (archive == archives_.end()) {
        AnswerOpen(request, {.result = InvalidArchiveHandle, .file = nullptr});
        return;
    }
    AnswerOpen(request, OpenIn(archive->second, path));
}

void FileSystem::OpenFileDirectly(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the transaction
    const std::uint32_t archive = request.Pop();
    static_cast<void>(request.Pop());  // the archive path's type
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.Pop());  // the file path's type
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.Pop());  // the open mode
    static_cast<void>(request.Pop());  // attributes
    static_cast<void>(request.PopStaticBuffer());
    const std::vector<std::uint8_t> path = request.PopStaticBuffer();

    const kernel::ResultCode open = CanOpen(archive);
    if (open.IsError()) {
        AnswerOpen(request, {.result = open, .file = nullptr});
        return;
    }
    AnswerOpen(request, OpenIn(archive, path));
}

void FileSystem::OpenArchive(kernel::IpcRequest& request) {
    const std::uint32_t archive = request.Pop();
    static_cast<void>(request.Pop());  // the path's type
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.PopStaticBuffer());

    const kernel::ResultCode open = CanOpen(archive);
    request.Begin(3, 0);
    request.Push(open);
    if (open.IsError()) {
        request.Push64(0);
        return;
    }
    const std::uint64_t handle = next_archive_;
    ++next_archive_;
    archives_.emplace(handle, archive);
    request.Push64(handle);
}

void FileSystem::CloseArchive(kernel::IpcRequest& request) {
    const std::uint64_t handle = request.Pop64();
    request.ReplyWith(archives_.erase(handle) == 0 ? InvalidArchiveHandle
                                                   : kernel::result::Success);
}

void FileSystem::GetFormatInfo(kernel::IpcRequest& request) {
    const std::uint32_t archive = request.Pop();
    static_cast<void>(request.Pop());  // the path's type
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.PopStaticBuffer());

    // Total size, directory count, file count and whether data is duplicated,
    // all zero when the archive has none.
    request.Begin(FormatInfoReplyWords, 0);
    request.Push(archive == SaveDataArchive ? NotFormatted : NotFound);
    request.Push(0);
    request.Push(0);
    request.Push(0);
    request.Push(0);
}

OpenFileSession::OpenFileSession(core::ByteSpan contents) : contents_(contents) {}

std::span<const OpenFileSession::Command> OpenFileSession::Commands() {
    static constexpr std::array<Command, 9> Table = {{
        {.id = 0x0801, .name = "OpenSubFile"},
        {.id = 0x0802, .name = "Read", .handler = &OpenFileSession::Read},
        {.id = 0x0803, .name = "Write"},
        {.id = 0x0804, .name = "GetSize", .handler = &OpenFileSession::GetSize},
        {.id = 0x0805, .name = "SetSize"},
        {.id = 0x0808, .name = "Close", .handler = &OpenFileSession::Close},
        {.id = 0x0809, .name = "Flush"},
        {.id = 0x080A, .name = "SetPriority", .handler = &OpenFileSession::SetPriority},
        {.id = 0x080B, .name = "GetPriority", .handler = &OpenFileSession::GetPriority},
    }};
    return Table;
}

void OpenFileSession::Read(kernel::IpcRequest& request) {
    const std::uint64_t offset = request.Pop64();
    const std::uint32_t length = request.Pop();
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();

    // A read past the end returns what there is, as Azahar's backends do.
    const std::size_t start = std::min<std::uint64_t>(offset, contents_.size());
    const std::size_t count = std::min<std::size_t>(length, contents_.size() - start);
    const bool copied = request.GetKernel()
                            .Space()
                            .Memory()
                            .Load(buffer.address, contents_.subspan(start, count))
                            .Ok();

    request.Begin(2, 2);
    request.Push(copied ? kernel::result::Success : kernel::result::InvalidPointer);
    request.Push(copied ? static_cast<std::uint32_t>(count) : 0);
    request.PushMappedBuffer(buffer);
}

void OpenFileSession::GetSize(kernel::IpcRequest& request) {
    request.Begin(3, 0);
    request.Push(kernel::result::Success);
    request.Push64(contents_.size());
}

void OpenFileSession::Close(kernel::IpcRequest& request) {
    closed_ = true;
    request.ReplyWith(kernel::result::Success);
}

void OpenFileSession::SetPriority(kernel::IpcRequest& request) {
    priority_ = request.Pop();
    request.ReplyWith(kernel::result::Success);
}

void OpenFileSession::GetPriority(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(priority_);
}

}  // namespace hexlith::domain::services
