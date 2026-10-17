#include "domain/services/file_system.h"

#include "core/byte_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace hexlith::domain::services {
namespace {

// GetFormatInfo answers four words after its result.
constexpr std::uint32_t FormatInfoReplyWords = 5;

// A SelfNCCH file path is twelve bytes: a type word, then an ExeFS file name.
// Type 0 is the RomFS. Source: SelfNCCHFilePath in Azahar.
constexpr std::size_t SelfNcchPathSize = 12;
constexpr std::uint32_t SelfNcchRomFs = 0;

// Path types. Source: LowPathType in Azahar's archive_backend.h.
constexpr std::uint32_t EmptyPath = 1;

// FormatSaveData counts the archive's size in blocks of this many bytes.
// Source: FS_USER::FormatSaveData in Azahar.
constexpr std::uint32_t SaveDataBlockSize = 512;

std::shared_ptr<kernel::Object> AsObject(const std::shared_ptr<kernel::SessionHandler>& file) {
    if (file == nullptr) {
        return nullptr;
    }
    return std::make_shared<kernel::ClientSession>(file);
}

// A new session moved to the caller, as every command that opens something
// answers.
void AnswerWithSession(kernel::IpcRequest& request,
                       const std::shared_ptr<kernel::SessionHandler>& session) {
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {AsObject(session)};
    request.PushMoveObjects(objects);
}

}  // namespace

FileSystem::FileSystem(core::ByteSpan romfs,
                       std::shared_ptr<SaveData> save,
                       std::shared_ptr<ExtraData> extra)
    : romfs_(romfs), save_(std::move(save)), extra_(std::move(extra)) {}

std::span<const FileSystem::Command> FileSystem::Commands() {
    static constexpr std::array<Command, 18> Table = {{
        {.id = 0x0801, .name = "Initialize", .handler = &FileSystem::Initialize},
        {.id = 0x0802, .name = "OpenFile", .handler = &FileSystem::OpenFile},
        {.id = 0x0803, .name = "OpenFileDirectly", .handler = &FileSystem::OpenFileDirectly},
        {.id = 0x0804, .name = "DeleteFile", .handler = &FileSystem::DeleteFile},
        {.id = 0x0808, .name = "CreateFile", .handler = &FileSystem::CreateFile},
        {.id = 0x0809, .name = "CreateDirectory", .handler = &FileSystem::CreateDirectory},
        {.id = 0x080B, .name = "OpenDirectory", .handler = &FileSystem::OpenDirectory},
        {.id = 0x080C, .name = "OpenArchive", .handler = &FileSystem::OpenArchive},
        {.id = 0x080D, .name = "ControlArchive", .handler = &FileSystem::ControlArchive},
        {.id = 0x080E, .name = "CloseArchive", .handler = &FileSystem::CloseArchive},
        {.id = 0x080F,
         .name = "FormatThisUserSaveData",
         .handler = &FileSystem::FormatThisUserSaveData},
        {.id = 0x0845, .name = "GetFormatInfo", .handler = &FileSystem::GetFormatInfo},
        {.id = 0x084C, .name = "FormatSaveData", .handler = &FileSystem::FormatSaveData},
        {.id = 0x0851, .name = "CreateExtSaveData", .handler = &FileSystem::CreateExtSaveData},
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

FileSystem::MountResult FileSystem::Mount(std::uint32_t archive,
                                          std::span<const std::uint8_t> path) const {
    switch (archive) {
    case SelfNcchArchive:
        return {.result = kernel::result::Success,
                .mounted = {.archive = archive, .data = nullptr}};
    case SaveDataArchive:
        // On a first boot the title has never formatted its save data.
        if (!save_->IsFormatted()) {
            return {.result = NotFormatted, .mounted = {}};
        }
        return {.result = kernel::result::Success, .mounted = {.archive = archive, .data = save_}};
    case ExtraDataArchive: {
        // Extra data that was never created answers with a different summary
        // from the system archives, as Azahar notes the console does.
        const std::optional<ExtraDataId> id = ExtraDataIdFromPath(path);
        if (!id.has_value()) {
            return {.result = InvalidPath, .mounted = {}};
        }
        const auto found = extra_->find(*id);
        if (found == extra_->end()) {
            return {.result = ExtraDataNotFound, .mounted = {}};
        }
        return {.result = kernel::result::Success,
                .mounted = {.archive = archive, .data = found->second}};
    }
    default:
        // The system archives and the rest: nothing here holds them, and the
        // console's own system files are not on the cartridge.
        return {.result = NotFound, .mounted = {}};
    }
}

FileSystem::OpenResult FileSystem::OpenIn(const Mounted& archive,
                                          std::uint32_t path_type,
                                          std::span<const std::uint8_t> path,
                                          OpenMode mode) const {
    if (archive.data != nullptr) {
        const std::optional<std::string> name = SaveDataPath(path_type, path);
        if (!name.has_value()) {
            return {.result = InvalidPath, .file = nullptr};
        }
        const kernel::ResultCode opened = archive.data->Open(*name, mode);
        if (opened.IsError()) {
            return {.result = opened, .file = nullptr};
        }
        return {.result = kernel::result::Success,
                .file = std::make_shared<SaveFileSession>(archive.data, *name, mode)};
    }
    if (archive.archive != SelfNcchArchive) {
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
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // its size
    const OpenMode mode = OpenMode::FromFlags(request.Pop());
    static_cast<void>(request.Pop());  // attributes
    const std::vector<std::uint8_t> path = request.PopStaticBuffer();

    const auto archive = archives_.find(handle);
    if (archive == archives_.end()) {
        AnswerOpen(request, {.result = InvalidArchiveHandle, .file = nullptr});
        return;
    }
    AnswerOpen(request, OpenIn(archive->second, path_type, path, mode));
}

void FileSystem::OpenFileDirectly(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the transaction
    const std::uint32_t archive = request.Pop();
    static_cast<void>(request.Pop());  // the archive path's type
    static_cast<void>(request.Pop());  // its size
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // its size
    const OpenMode mode = OpenMode::FromFlags(request.Pop());
    static_cast<void>(request.Pop());  // attributes
    const std::vector<std::uint8_t> archive_path = request.PopStaticBuffer();
    const std::vector<std::uint8_t> path = request.PopStaticBuffer();

    const MountResult mount = Mount(archive, archive_path);
    if (mount.result.IsError()) {
        AnswerOpen(request, {.result = mount.result, .file = nullptr});
        return;
    }
    AnswerOpen(request, OpenIn(mount.mounted, path_type, path, mode));
}

void FileSystem::OpenArchive(kernel::IpcRequest& request) {
    const std::uint32_t archive = request.Pop();
    static_cast<void>(request.Pop());  // the path's type
    static_cast<void>(request.Pop());  // its size
    const std::vector<std::uint8_t> path = request.PopStaticBuffer();

    const MountResult mount = Mount(archive, path);
    request.Begin(3, 0);
    request.Push(mount.result);
    if (mount.result.IsError()) {
        request.Push64(0);
        return;
    }
    const std::uint64_t handle = next_archive_;
    ++next_archive_;
    archives_.emplace(handle, mount.mounted);
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
    const std::vector<std::uint8_t> path = request.PopStaticBuffer();

    // A writable archive that exists answers with what it was formatted
    // with. One that does not is not formatted, whichever kind it is, which
    // is what Azahar answers for missing metadata.
    const MountResult mount = Mount(archive, path);
    kernel::ResultCode result = mount.result;
    SaveDataFormat format;
    if (mount.mounted.data != nullptr) {
        format = mount.mounted.data->Format().value_or(SaveDataFormat{});
    } else if (archive == ExtraDataArchive && result == ExtraDataNotFound) {
        result = NotFormatted;
    } else if (result == kernel::result::Success) {
        result = NotFound;
    }

    // Total size, directory count, file count and whether data is duplicated,
    // all zero when the archive has none.
    request.Begin(FormatInfoReplyWords, 0);
    request.Push(result);
    request.Push(format.total_size);
    request.Push(format.directories);
    request.Push(format.files);
    request.Push(format.duplicate_data ? 1 : 0);
}

void FileSystem::FormatSaveData(kernel::IpcRequest& request) {
    const std::uint32_t archive = request.Pop();
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // the path's size
    const std::uint32_t blocks = request.Pop();
    const std::uint32_t directories = request.Pop();
    const std::uint32_t files = request.Pop();
    static_cast<void>(request.Pop());  // directory buckets
    static_cast<void>(request.Pop());  // file buckets
    const bool duplicate_data = (request.Pop() & 0xFFU) != 0;
    static_cast<void>(request.PopStaticBuffer());

    // Only the title's own save data, named by an empty path, as in Azahar.
    if (archive != SaveDataArchive) {
        request.ReplyWith(InvalidPath);
        return;
    }
    if (path_type != EmptyPath) {
        request.ReplyWith(Unsupported);
        return;
    }
    save_->Reformat(SaveDataFormat{.total_size = blocks * SaveDataBlockSize,
                                   .directories = directories,
                                   .files = files,
                                   .duplicate_data = duplicate_data});
    request.ReplyWith(kernel::result::Success);
}

void FileSystem::FormatThisUserSaveData(kernel::IpcRequest& request) {
    const std::uint32_t blocks = request.Pop();
    const std::uint32_t directories = request.Pop();
    const std::uint32_t files = request.Pop();
    static_cast<void>(request.Pop());  // directory buckets
    static_cast<void>(request.Pop());  // file buckets
    const bool duplicate_data = (request.Pop() & 0xFFU) != 0;

    save_->Reformat(SaveDataFormat{.total_size = blocks * SaveDataBlockSize,
                                   .directories = directories,
                                   .files = files,
                                   .duplicate_data = duplicate_data});
    request.ReplyWith(kernel::result::Success);
}

FileSystem::SavePath FileSystem::InSaveData(std::uint64_t handle,
                                            std::uint32_t type,
                                            std::span<const std::uint8_t> bytes) const {
    const auto archive = archives_.find(handle);
    if (archive == archives_.end()) {
        return {.result = InvalidArchiveHandle, .data = nullptr, .path = {}};
    }
    if (archive->second.data == nullptr) {
        // The RomFS and the rest are read-only here.
        return {.result = Unsupported, .data = nullptr, .path = {}};
    }
    std::optional<std::string> path = SaveDataPath(type, bytes);
    if (!path.has_value()) {
        return {.result = InvalidPath, .data = nullptr, .path = {}};
    }
    return {
        .result = kernel::result::Success, .data = archive->second.data, .path = std::move(*path)};
}

void FileSystem::CreateFile(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the transaction
    const std::uint64_t handle = request.Pop64();
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.Pop());  // attributes
    const std::uint64_t size = request.Pop64();
    const std::vector<std::uint8_t> bytes = request.PopStaticBuffer();

    const SavePath path = InSaveData(handle, path_type, bytes);
    request.ReplyWith(path.result.IsError() ? path.result : path.data->CreateFile(path.path, size));
}

void FileSystem::DeleteFile(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the transaction
    const std::uint64_t handle = request.Pop64();
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // its size
    const std::vector<std::uint8_t> bytes = request.PopStaticBuffer();

    const SavePath path = InSaveData(handle, path_type, bytes);
    request.ReplyWith(path.result.IsError() ? path.result : path.data->DeleteFile(path.path));
}

void FileSystem::CreateDirectory(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());  // the transaction
    const std::uint64_t handle = request.Pop64();
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // its size
    static_cast<void>(request.Pop());  // attributes
    const std::vector<std::uint8_t> bytes = request.PopStaticBuffer();

    const SavePath path = InSaveData(handle, path_type, bytes);
    request.ReplyWith(path.result.IsError() ? path.result : path.data->CreateDirectory(path.path));
}

void FileSystem::ControlArchive(kernel::IpcRequest& request) {
    const std::uint64_t handle = request.Pop64();
    const std::uint32_t action = request.Pop();
    static_cast<void>(request.Pop());  // the input's size
    static_cast<void>(request.Pop());  // the output's size
    static_cast<void>(request.PopMappedBuffer());
    static_cast<void>(request.PopMappedBuffer());

    const auto archive = archives_.find(handle);
    if (archive == archives_.end()) {
        request.ReplyWith(InvalidArchiveHandle);
        return;
    }
    // Azahar answers every action with success and writes nothing back, and
    // the commit is the only one with an effect here.
    if (archive->second.archive == SaveDataArchive && action == CommitSaveData) {
        save_->Commit();
    }
    request.ReplyWith(kernel::result::Success);
}

void FileSystem::OpenDirectory(kernel::IpcRequest& request) {
    const std::uint64_t handle = request.Pop64();
    const std::uint32_t path_type = request.Pop();
    static_cast<void>(request.Pop());  // its size
    const std::vector<std::uint8_t> bytes = request.PopStaticBuffer();

    const SavePath path = InSaveData(handle, path_type, bytes);
    std::shared_ptr<kernel::SessionHandler> directory;
    kernel::ResultCode result = path.result;
    if (!result.IsError()) {
        SaveData::Listing listing = path.data->List(path.path);
        result = listing.result;
        if (!result.IsError()) {
            directory = std::make_shared<DirectorySession>(std::move(listing.entries));
        }
    }
    AnswerOpen(request, {.result = result, .file = directory});
}

void FileSystem::CreateExtSaveData(kernel::IpcRequest& request) {
    // The first word packs the media type into its low byte. Source:
    // ExtSaveDataInfo in Azahar's fs_user.h.
    const std::uint32_t media = request.Pop() & 0xFFU;
    const std::uint32_t low = request.Pop();
    const std::uint32_t high = request.Pop();
    static_cast<void>(request.Pop());  // reserved
    const std::uint32_t directories = request.Pop();
    const std::uint32_t files = request.Pop();
    const std::uint64_t size_limit = request.Pop64();
    static_cast<void>(request.Pop());  // the icon's size
    const kernel::MappedBuffer icon = request.PopMappedBuffer();

    // A second creation keeps the files and takes the new format, as
    // Azahar's FormatAsExtData does. The icon is not kept: nothing on the way
    // to the title screen reads it back.
    const SaveDataFormat format{.total_size = static_cast<std::uint32_t>(size_limit),
                                .directories = directories,
                                .files = files,
                                .duplicate_data = false};
    const ExtraDataId id{.media_type = media, .low = low, .high = high};
    std::shared_ptr<SaveData>& data = (*extra_)[id];
    if (data == nullptr) {
        data = std::make_shared<SaveData>(ArchiveKind::ExtraData);
        data->Reformat(format);
    } else {
        data->Restore(format, data->Directories(), data->Files());
    }

    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    request.PushMappedBuffer(icon);
}

OpenFileSession::OpenFileSession(core::ByteSpan contents) : contents_(contents) {}

std::span<const OpenFileSession::Command> OpenFileSession::Commands() {
    static constexpr std::array<Command, 10> Table = {{
        {.id = 0x0801, .name = "OpenSubFile"},
        {.id = 0x0802, .name = "Read", .handler = &OpenFileSession::Read},
        {.id = 0x0803, .name = "Write"},
        {.id = 0x0804, .name = "GetSize", .handler = &OpenFileSession::GetSize},
        {.id = 0x0805, .name = "SetSize"},
        {.id = 0x0808, .name = "Close", .handler = &OpenFileSession::Close},
        {.id = 0x0809, .name = "Flush"},
        {.id = 0x080A, .name = "SetPriority", .handler = &OpenFileSession::SetPriority},
        {.id = 0x080B, .name = "GetPriority", .handler = &OpenFileSession::GetPriority},
        {.id = 0x080C, .name = "OpenLinkFile", .handler = &OpenFileSession::OpenLinkFile},
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

// A second session on the same file, with the priority of this one. Source:
// File::OpenLinkFile in Azahar.
void OpenFileSession::OpenLinkFile(kernel::IpcRequest& request) {
    auto link = std::make_shared<OpenFileSession>(contents_);
    link->priority_ = priority_;
    AnswerWithSession(request, link);
}

DirectorySession::DirectorySession(std::vector<SaveData::Entry> entries)
    : entries_(std::move(entries)) {}

std::span<const DirectorySession::Command> DirectorySession::Commands() {
    static constexpr std::array<Command, 2> Table = {{
        {.id = 0x0801, .name = "Read", .handler = &DirectorySession::Read},
        {.id = 0x0802, .name = "Close", .handler = &DirectorySession::Close},
    }};
    return Table;
}

std::vector<std::uint8_t> DirectorySession::Encode(const SaveData::Entry& entry) {
    // Offsets of the fields. Source: Entry in Azahar's directory_backend.h.
    constexpr std::size_t NameUnits = 0x20C / 2;
    constexpr std::size_t ShortNameOffset = 0x20C;
    constexpr std::size_t ShortNameLength = 8;
    constexpr std::size_t ExtensionOffset = 0x216;
    constexpr std::size_t ExtensionLength = 3;
    constexpr std::size_t DirectoryFlagOffset = 0x21C;
    constexpr std::size_t HiddenFlagOffset = 0x21D;
    constexpr std::size_t ArchiveFlagOffset = 0x21E;
    constexpr std::size_t SizeOffset = 0x220;

    std::vector<std::uint8_t> bytes(EntrySize, 0);

    // The long name, cut short to leave room for its terminator.
    const std::u16string name = Utf16Of(entry.name);
    const std::size_t units = std::min(name.size(), NameUnits - 1);
    for (std::size_t index = 0; index < units; ++index) {
        bytes.at(index * 2) = static_cast<std::uint8_t>(name[index]);
        bytes.at((index * 2) + 1) = static_cast<std::uint8_t>(name[index] >> 8U);
    }

    // The 8.3 name the way Azahar's SplitFilename83 builds it: padded with
    // spaces, upper case, without the characters FAT refuses, and with ~1 when
    // the name is too long.
    std::fill_n(bytes.begin() + ShortNameOffset, ShortNameLength, ' ');
    std::fill_n(bytes.begin() + ExtensionOffset, ExtensionLength, ' ');
    constexpr std::string_view Forbidden = ".\"/\\[]:;=, ";
    std::size_t point = entry.name.rfind('.');
    if (point != std::string::npos && point + 1 == entry.name.size()) {
        point = entry.name.rfind('.', point);
    }
    std::size_t length = 0;
    for (const char letter : std::string_view{entry.name}.substr(0, point)) {
        if (Forbidden.find(letter) != std::string_view::npos) {
            continue;
        }
        if (length == ShortNameLength) {
            bytes.at(ShortNameOffset + ShortNameLength - 2) = '~';
            bytes.at(ShortNameOffset + ShortNameLength - 1) = '1';
            break;
        }
        bytes.at(ShortNameOffset + length) = static_cast<std::uint8_t>(std::toupper(letter));
        ++length;
    }
    if (point != std::string::npos) {
        const std::string_view extension =
            std::string_view{entry.name}.substr(point + 1, ExtensionLength);
        for (std::size_t index = 0; index < extension.size(); ++index) {
            bytes.at(ExtensionOffset + index) =
                static_cast<std::uint8_t>(std::toupper(extension[index]));
        }
    }

    // Azahar sets the archive bit on every file, as on an SD card where it
    // was never cleared, and calls a name that starts with a dot hidden.
    bytes.at(DirectoryFlagOffset) = entry.is_directory ? 1 : 0;
    bytes.at(HiddenFlagOffset) = entry.name.starts_with('.') ? 1 : 0;
    bytes.at(ArchiveFlagOffset) = entry.is_directory ? 0 : 1;
    for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
        bytes.at(SizeOffset + byte) = static_cast<std::uint8_t>(entry.size >> (8U * byte));
    }
    return bytes;
}

void DirectorySession::Read(kernel::IpcRequest& request) {
    const std::uint32_t count = request.Pop();
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();

    std::vector<std::uint8_t> bytes;
    std::uint32_t read = 0;
    while (read < count && next_ < entries_.size()) {
        const std::vector<std::uint8_t> entry = Encode(entries_.at(next_));
        bytes.insert(bytes.end(), entry.begin(), entry.end());
        ++next_;
        ++read;
    }
    const bool copied = request.GetKernel().Space().Memory().Load(buffer.address, bytes).Ok();

    request.Begin(2, 2);
    request.Push(copied ? kernel::result::Success : kernel::result::InvalidPointer);
    request.Push(copied ? read : 0);
    request.PushMappedBuffer(buffer);
}

void DirectorySession::Close(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

SaveFileSession::SaveFileSession(std::shared_ptr<SaveData> save, std::string path, OpenMode mode)
    : save_(std::move(save)), path_(std::move(path)), mode_(mode) {
    // Extra data opens every file for reading and writing, whatever was asked.
    if (save_->Kind() == ArchiveKind::ExtraData) {
        mode_.read = true;
        mode_.write = true;
    }
}

std::span<const SaveFileSession::Command> SaveFileSession::Commands() {
    static constexpr std::array<Command, 10> Table = {{
        {.id = 0x0801, .name = "OpenSubFile"},
        {.id = 0x0802, .name = "Read", .handler = &SaveFileSession::Read},
        {.id = 0x0803, .name = "Write", .handler = &SaveFileSession::Write},
        {.id = 0x0804, .name = "GetSize", .handler = &SaveFileSession::GetSize},
        {.id = 0x0805, .name = "SetSize", .handler = &SaveFileSession::SetSize},
        {.id = 0x0808, .name = "Close", .handler = &SaveFileSession::Close},
        {.id = 0x0809, .name = "Flush", .handler = &SaveFileSession::Flush},
        {.id = 0x080A, .name = "SetPriority", .handler = &SaveFileSession::SetPriority},
        {.id = 0x080B, .name = "GetPriority", .handler = &SaveFileSession::GetPriority},
        {.id = 0x080C, .name = "OpenLinkFile", .handler = &SaveFileSession::OpenLinkFile},
    }};
    return Table;
}

void SaveFileSession::Read(kernel::IpcRequest& request) {
    const std::uint64_t offset = request.Pop64();
    const std::uint32_t length = request.Pop();
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();

    request.Begin(2, 2);
    if (!mode_.read) {
        request.Push(save_result::InvalidOpenFlags);
        request.Push(0);
        request.PushMappedBuffer(buffer);
        return;
    }
    const std::span<const std::uint8_t> contents = save_->Read(path_, offset, length);
    const bool copied = request.GetKernel().Space().Memory().Load(buffer.address, contents).Ok();
    request.Push(copied ? kernel::result::Success : kernel::result::InvalidPointer);
    request.Push(copied ? static_cast<std::uint32_t>(contents.size()) : 0);
    request.PushMappedBuffer(buffer);
}

void SaveFileSession::Write(kernel::IpcRequest& request) {
    const std::uint64_t offset = request.Pop64();
    const std::uint32_t length = request.Pop();
    static_cast<void>(request.Pop());  // whether to flush, and to update the time stamp
    const kernel::MappedBuffer buffer = request.PopMappedBuffer();

    request.Begin(2, 2);
    if (!mode_.write) {
        request.Push(save_result::InvalidOpenFlags);
        request.Push(0);
        request.PushMappedBuffer(buffer);
        return;
    }
    std::vector<std::uint8_t> data(length);
    if (!request.GetKernel().Space().Memory().Copy(buffer.address, data).Ok()) {
        request.Push(kernel::result::InvalidPointer);
        request.Push(0);
        request.PushMappedBuffer(buffer);
        return;
    }
    const SaveData::Written written = save_->Write(path_, offset, data);
    request.Push(written.result);
    request.Push(written.count);
    request.PushMappedBuffer(buffer);
}

void SaveFileSession::GetSize(kernel::IpcRequest& request) {
    request.Begin(3, 0);
    request.Push(kernel::result::Success);
    request.Push64(save_->SizeOf(path_));
}

void SaveFileSession::SetSize(kernel::IpcRequest& request) {
    const std::uint64_t size = request.Pop64();
    if (!mode_.write) {
        request.ReplyWith(save_result::InvalidOpenFlags);
        return;
    }
    save_->Resize(path_, size);
    request.ReplyWith(kernel::result::Success);
}

void SaveFileSession::Close(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

// Writes reach the archive at once, so there is nothing to flush. Durability
// is the commit's business.
void SaveFileSession::Flush(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

void SaveFileSession::SetPriority(kernel::IpcRequest& request) {
    priority_ = request.Pop();
    request.ReplyWith(kernel::result::Success);
}

void SaveFileSession::GetPriority(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(priority_);
}

void SaveFileSession::OpenLinkFile(kernel::IpcRequest& request) {
    auto link = std::make_shared<SaveFileSession>(save_, path_, mode_);
    link->priority_ = priority_;
    AnswerWithSession(request, link);
}

}  // namespace hexlith::domain::services
