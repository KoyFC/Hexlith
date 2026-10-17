#include "domain/services/file_system.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <numeric>
#include <string>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// fs:USER and file command headers. Source: 3dbrew, Filesystem services and
// FS:OpenFile.
constexpr std::uint32_t OpenFile = 0x080201C2;
constexpr std::uint32_t OpenFileDirectly = 0x08030204;
constexpr std::uint32_t OpenArchive = 0x080C00C2;
constexpr std::uint32_t CloseArchive = 0x080E0080;
constexpr std::uint32_t GetFormatInfo = 0x084500C2;
constexpr std::uint32_t Read = 0x080200C2;
constexpr std::uint32_t GetSize = 0x08040000;
constexpr std::uint32_t Write = 0x08030102;
constexpr std::uint32_t CreateFile = 0x08080202;
constexpr std::uint32_t CreateDirectory = 0x08090182;
constexpr std::uint32_t OpenDirectory = 0x080B0102;
constexpr std::uint32_t ControlArchive = 0x080D0144;
constexpr std::uint32_t FormatSaveData = 0x084C0242;
constexpr std::uint32_t CreateExtSaveData = 0x08510242;
constexpr std::uint32_t ReadDirectory = 0x08010042;
constexpr std::uint32_t OpenLinkFile = 0x080C0000;

// Path types and the binary RomFS path of SelfNCCH. Source: 3dbrew,
// Filesystem services, Paths.
constexpr std::uint32_t EmptyPath = 1;
constexpr std::uint32_t BinaryPath = 2;
constexpr std::uint32_t AsciiPath = 3;
constexpr std::uint32_t ReadWriteMode = 3;
constexpr std::uint32_t ReadAccess = 1;
constexpr std::uint32_t RomFsPathSize = 12;
constexpr std::uint32_t ReadMode = 1;
constexpr std::uint32_t WriteAccess = 2;

// An archive ID nothing here holds: the shared extra data archive.
constexpr std::uint32_t SharedExtraData = 0x00000007;

constexpr std::uint32_t PathAddress = ServiceHarness::BufferAddress;
constexpr std::uint32_t DataAddress = ServiceHarness::BufferAddress + 0x100;
constexpr std::uint32_t NameAddress = ServiceHarness::BufferAddress + 0x40;
constexpr std::uint32_t ExtraPathAddress = ServiceHarness::BufferAddress + 0x80;
constexpr std::uint32_t ExtraPathSize = 12;

// An ASCII path as the game sends it, with its terminator.
std::vector<std::uint8_t> AsciiBytes(const std::string& text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
}

std::vector<std::uint8_t> CountingBytes(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

struct Harness {
    Harness() : romfs(CountingBytes(0x40)), service(romfs, save, extra) {
        // A zero RomFS path, and an empty archive path at the same address.
        harness.WriteBytes(PathAddress, std::vector<std::uint8_t>(RomFsPathSize, 0));
    }

    // Opens the RomFS directly and answers the file session it hands back.
    std::shared_ptr<kernel::ClientSession> OpenRomFs() {
        EXPECT_TRUE(harness.Send(service,
                                 {OpenFileDirectly,
                                  0,
                                  FileSystem::SelfNcchArchive,
                                  EmptyPath,
                                  1,
                                  BinaryPath,
                                  RomFsPathSize,
                                  ReadMode,
                                  0,
                                  ipc::StaticBufferDescriptorFor(1, 2),
                                  PathAddress,
                                  ipc::StaticBufferDescriptorFor(RomFsPathSize, 0),
                                  PathAddress}));
        EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
        EXPECT_EQ(harness.Reply(2), ipc::MoveHandlesDescriptor(1));
        return harness.ReplyObject<kernel::ClientSession>(3, kernel::ObjectType::ClientSession);
    }

    std::uint32_t OpenArchiveWith(std::uint32_t archive) {
        EXPECT_TRUE(harness.Send(service,
                                 {OpenArchive,
                                  archive,
                                  EmptyPath,
                                  1,
                                  ipc::StaticBufferDescriptorFor(1, 0),
                                  PathAddress}));
        return harness.Reply(1);
    }

    // Formats the save data with 512-byte blocks and opens it.
    std::uint32_t FormatAndOpenSaveData(std::uint32_t blocks) {
        EXPECT_TRUE(harness.Send(service,
                                 {FormatSaveData,
                                  FileSystem::SaveDataArchive,
                                  EmptyPath,
                                  1,
                                  blocks,
                                  2,
                                  3,
                                  4,
                                  5,
                                  1,
                                  ipc::StaticBufferDescriptorFor(1, 0),
                                  PathAddress}));
        EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
        EXPECT_EQ(OpenArchiveWith(FileSystem::SaveDataArchive), kernel::result::Success.raw);
        return harness.Reply(2);
    }

    // Names a path at NameAddress, which the next request refers to.
    std::uint32_t Name(const std::string& path) {
        const std::vector<std::uint8_t> bytes = AsciiBytes(path);
        harness.WriteBytes(NameAddress, bytes);
        return static_cast<std::uint32_t>(bytes.size());
    }

    kernel::ResultCode
    CreateFileIn(std::uint32_t archive, const std::string& path, std::uint32_t size) {
        const std::uint32_t length = Name(path);
        EXPECT_TRUE(harness.Send(service,
                                 {CreateFile,
                                  0,
                                  archive,
                                  0,
                                  AsciiPath,
                                  length,
                                  0,
                                  size,
                                  0,
                                  ipc::StaticBufferDescriptorFor(length, 0),
                                  NameAddress}));
        return kernel::ResultCode{harness.Reply(1)};
    }

    std::shared_ptr<kernel::ClientSession>
    OpenIn(std::uint32_t archive, const std::string& path, std::uint32_t mode) {
        const std::uint32_t length = Name(path);
        EXPECT_TRUE(harness.Send(service,
                                 {OpenFile,
                                  0,
                                  archive,
                                  0,
                                  AsciiPath,
                                  length,
                                  mode,
                                  0,
                                  ipc::StaticBufferDescriptorFor(length, 0),
                                  NameAddress}));
        EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
        return harness.ReplyObject<kernel::ClientSession>(3, kernel::ObjectType::ClientSession);
    }

    std::uint32_t OpenExtraData(std::uint32_t low) {
        const std::vector<std::uint8_t> path = {1,
                                                0,
                                                0,
                                                0,
                                                static_cast<std::uint8_t>(low),
                                                static_cast<std::uint8_t>(low >> 8U),
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0};
        harness.WriteBytes(ExtraPathAddress, path);
        EXPECT_TRUE(harness.Send(service,
                                 {OpenArchive,
                                  FileSystem::ExtraDataArchive,
                                  BinaryPath,
                                  ExtraPathSize,
                                  ipc::StaticBufferDescriptorFor(ExtraPathSize, 0),
                                  ExtraPathAddress}));
        return harness.Reply(1);
    }

    ServiceHarness harness;
    std::vector<std::uint8_t> romfs;
    std::shared_ptr<SaveData> save = std::make_shared<SaveData>();
    std::shared_ptr<ExtraData> extra = std::make_shared<ExtraData>();
    FileSystem service;
};

TEST(FileSystem, OpensTheRomFsDirectlyAndReadsIt) {
    Harness fs;
    const auto file = fs.OpenRomFs();
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->handler->Name(), "fs:file");

    constexpr std::uint32_t Offset = 0x10;
    constexpr std::uint32_t Length = 8;
    ASSERT_TRUE(fs.harness.Send(*file->handler,
                                {Read,
                                 Offset,
                                 0,
                                 Length,
                                 ipc::MappedBufferDescriptorFor(Length, WriteAccess),
                                 DataAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(fs.harness.Reply(2), Length);
    EXPECT_EQ(
        fs.harness.ReadBytes(DataAddress, Length),
        std::vector<std::uint8_t>(fs.romfs.begin() + Offset, fs.romfs.begin() + Offset + Length));
}

TEST(FileSystem, ReadsNoMoreThanTheFileHolds) {
    Harness fs;
    const auto file = fs.OpenRomFs();
    ASSERT_NE(file, nullptr);
    constexpr std::uint32_t Offset = 0x3C;
    constexpr std::uint32_t Length = 0x10;
    ASSERT_TRUE(fs.harness.Send(*file->handler,
                                {Read,
                                 Offset,
                                 0,
                                 Length,
                                 ipc::MappedBufferDescriptorFor(Length, WriteAccess),
                                 DataAddress}));
    EXPECT_EQ(fs.harness.Reply(2), 4U);
}

TEST(FileSystem, AnswersTheSizeOfAFile) {
    Harness fs;
    const auto file = fs.OpenRomFs();
    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(fs.harness.Send(*file->handler, {GetSize}));
    EXPECT_EQ(fs.harness.Reply(2), fs.romfs.size());
    EXPECT_EQ(fs.harness.Reply(3), 0U);
}

TEST(FileSystem, OpensAFileInAnOpenArchive) {
    Harness fs;
    ASSERT_EQ(fs.OpenArchiveWith(FileSystem::SelfNcchArchive), kernel::result::Success.raw);
    const std::uint32_t handle = fs.harness.Reply(2);
    EXPECT_EQ(handle, 1U) << "archive handles count up from one";

    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {OpenFile,
                                 0,
                                 handle,
                                 0,
                                 BinaryPath,
                                 RomFsPathSize,
                                 ReadMode,
                                 0,
                                 ipc::StaticBufferDescriptorFor(RomFsPathSize, 0),
                                 PathAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_NE(fs.harness.ReplyObject<kernel::ClientSession>(3, kernel::ObjectType::ClientSession),
              nullptr);

    ASSERT_TRUE(fs.harness.Send(fs.service, {CloseArchive, handle, 0}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    ASSERT_TRUE(fs.harness.Send(fs.service, {CloseArchive, handle, 0}));
    EXPECT_EQ(fs.harness.Reply(1), FileSystem::InvalidArchiveHandle.raw);
}

TEST(FileSystem, RefusesAPathThatIsNotTheRomFs) {
    Harness fs;
    ASSERT_EQ(fs.OpenArchiveWith(FileSystem::SelfNcchArchive), kernel::result::Success.raw);
    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {OpenFile,
                                 0,
                                 fs.harness.Reply(2),
                                 0,
                                 BinaryPath,
                                 4,
                                 ReadMode,
                                 0,
                                 ipc::StaticBufferDescriptorFor(4, 0),
                                 PathAddress}));
    EXPECT_EQ(fs.harness.Reply(1), FileSystem::InvalidPath.raw);
}

TEST(FileSystem, SaysSaveDataIsNotFormattedOnAFirstBoot) {
    Harness fs;
    EXPECT_EQ(fs.OpenArchiveWith(FileSystem::SaveDataArchive), FileSystem::NotFormatted.raw);
    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {GetFormatInfo,
                                 FileSystem::SaveDataArchive,
                                 EmptyPath,
                                 1,
                                 ipc::StaticBufferDescriptorFor(1, 0),
                                 PathAddress}));
    EXPECT_EQ(fs.harness.Reply(0), ipc::MakeHeader(0x0845, 5, 0));
    EXPECT_EQ(fs.harness.Reply(1), FileSystem::NotFormatted.raw);
}

TEST(FileSystem, DoesNotFindArchivesTheCartridgeDoesNotHold) {
    Harness fs;
    EXPECT_EQ(fs.OpenArchiveWith(SharedExtraData), FileSystem::NotFound.raw);
}

TEST(FileSystem, FormatsSaveDataAndAnswersWithTheFormat) {
    Harness fs;
    static_cast<void>(fs.FormatAndOpenSaveData(8));
    ASSERT_TRUE(fs.save->IsFormatted());
    EXPECT_EQ(fs.save->Format().value_or(SaveDataFormat{}).total_size, 8U * 512U)
        << "the request counts 512-byte blocks";
    EXPECT_TRUE(fs.save->Format().value_or(SaveDataFormat{}).duplicate_data);

    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {GetFormatInfo,
                                 FileSystem::SaveDataArchive,
                                 EmptyPath,
                                 1,
                                 ipc::StaticBufferDescriptorFor(1, 0),
                                 PathAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(fs.harness.Reply(2), 8U * 512U);
    EXPECT_EQ(fs.harness.Reply(3), 2U);
    EXPECT_EQ(fs.harness.Reply(4), 3U);
    EXPECT_EQ(fs.harness.Reply(5), 1U);
}

TEST(FileSystem, WritesAndReadsBackAFileOfTheSaveData) {
    Harness fs;
    const std::uint32_t archive = fs.FormatAndOpenSaveData(8);
    ASSERT_EQ(fs.CreateFileIn(archive, "/data", 4), kernel::result::Success);
    const auto file = fs.OpenIn(archive, "/data", ReadWriteMode);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->handler->Name(), "fs:file");

    const std::vector<std::uint8_t> data = {1, 2, 3, 4, 5, 6};
    fs.harness.WriteBytes(DataAddress, data);
    ASSERT_TRUE(fs.harness.Send(
        *file->handler,
        {Write, 2, 0, 6, 0, ipc::MappedBufferDescriptorFor(6, ReadAccess), DataAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(fs.harness.Reply(2), 6U);

    ASSERT_TRUE(fs.harness.Send(*file->handler, {GetSize}));
    EXPECT_EQ(fs.harness.Reply(2), 8U) << "the write grew the file";

    ASSERT_TRUE(fs.harness.Send(
        *file->handler,
        {Read, 0, 0, 8, ipc::MappedBufferDescriptorFor(8, WriteAccess), DataAddress}));
    EXPECT_EQ(fs.harness.ReadBytes(DataAddress, 8),
              (std::vector<std::uint8_t>{0, 0, 1, 2, 3, 4, 5, 6}));
}

TEST(FileSystem, CommitsSaveDataWhenTheTitleAsks) {
    Harness fs;
    const std::uint32_t archive = fs.FormatAndOpenSaveData(8);
    int commits = 0;
    fs.save->OnCommit([&commits](const SaveData&) { ++commits; });

    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {ControlArchive,
                                 archive,
                                 0,
                                 FileSystem::CommitSaveData,
                                 0,
                                 0,
                                 ipc::MappedBufferDescriptorFor(0, ReadAccess),
                                 DataAddress,
                                 ipc::MappedBufferDescriptorFor(0, WriteAccess),
                                 DataAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(commits, 1);
}

TEST(FileSystem, CreatesExtraDataTheTitleCanOpen) {
    Harness fs;
    constexpr std::uint32_t Id = 0x1234;
    EXPECT_EQ(fs.OpenExtraData(Id), FileSystem::ExtraDataNotFound.raw);

    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {CreateExtSaveData,
                                 1,
                                 Id,
                                 0,
                                 0,
                                 5,
                                 6,
                                 0x10000,
                                 0,
                                 0,
                                 ipc::MappedBufferDescriptorFor(0, ReadAccess),
                                 DataAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);

    ASSERT_EQ(fs.OpenExtraData(Id), kernel::result::Success.raw);
    const std::uint32_t archive = fs.harness.Reply(2);
    ASSERT_EQ(fs.CreateFileIn(archive, "/fixed", 4), kernel::result::Success);
    EXPECT_EQ(fs.CreateFileIn(archive, "/empty", 0), save_result::UnsupportedOpenFlags);
    ASSERT_EQ(fs.extra->size(), 1U);
    EXPECT_EQ(fs.extra->begin()->second->Format().value_or(SaveDataFormat{}).files, 6U);
}

TEST(FileSystem, ListsADirectoryOfTheSaveData) {
    Harness fs;
    const std::uint32_t archive = fs.FormatAndOpenSaveData(8);
    ASSERT_EQ(fs.CreateFileIn(archive, "/long file name.text", 9), kernel::result::Success);
    const std::uint32_t length = fs.Name("/dir");
    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {CreateDirectory,
                                 0,
                                 archive,
                                 0,
                                 AsciiPath,
                                 length,
                                 0,
                                 ipc::StaticBufferDescriptorFor(length, 0),
                                 NameAddress}));
    ASSERT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);

    const std::uint32_t root = fs.Name("/");
    ASSERT_TRUE(fs.harness.Send(fs.service,
                                {OpenDirectory,
                                 archive,
                                 0,
                                 AsciiPath,
                                 root,
                                 ipc::StaticBufferDescriptorFor(root, 0),
                                 NameAddress}));
    ASSERT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    const auto directory =
        fs.harness.ReplyObject<kernel::ClientSession>(3, kernel::ObjectType::ClientSession);
    ASSERT_NE(directory, nullptr);

    constexpr std::uint32_t Room = 0xC00;
    ASSERT_TRUE(fs.harness.Send(
        *directory->handler,
        {ReadDirectory, 4, ipc::MappedBufferDescriptorFor(Room, WriteAccess), DataAddress}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    ASSERT_EQ(fs.harness.Reply(2), 2U);

    // The directory sorts first. The file's entry is the second one.
    constexpr std::uint32_t Second = DataAddress + DirectorySession::EntrySize;
    EXPECT_EQ(fs.harness.ReadBytes(DataAddress, 6),
              (std::vector<std::uint8_t>{'d', 0, 'i', 0, 'r', 0}));
    EXPECT_EQ(fs.harness.ReadBytes(DataAddress + 0x21C, 1), std::vector<std::uint8_t>{1});
    EXPECT_EQ(fs.harness.ReadBytes(Second + 0x20C, 8),
              (std::vector<std::uint8_t>{'L', 'O', 'N', 'G', 'F', 'I', '~', '1'}));
    EXPECT_EQ(fs.harness.ReadBytes(Second + 0x216, 3), (std::vector<std::uint8_t>{'T', 'E', 'X'}));
    EXPECT_EQ(fs.harness.Read32(Second + 0x220), 9U);
}

TEST(FileSystem, RefusesToWriteToTheRomFs) {
    Harness fs;
    ASSERT_EQ(fs.OpenArchiveWith(FileSystem::SelfNcchArchive), kernel::result::Success.raw);
    EXPECT_EQ(fs.CreateFileIn(fs.harness.Reply(2), "/file", 4), FileSystem::Unsupported);
}

TEST(FileSystem, OpensASecondSessionOnTheSameFile) {
    Harness fs;
    const auto file = fs.OpenRomFs();
    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(fs.harness.Send(*file->handler, {OpenLinkFile}));
    EXPECT_EQ(fs.harness.Reply(1), kernel::result::Success.raw);
    const auto link =
        fs.harness.ReplyObject<kernel::ClientSession>(3, kernel::ObjectType::ClientSession);
    ASSERT_NE(link, nullptr);
    EXPECT_NE(link->handler, file->handler);
    ASSERT_TRUE(fs.harness.Send(*link->handler, {GetSize}));
    EXPECT_EQ(fs.harness.Reply(2), fs.romfs.size());
}

}  // namespace
}  // namespace hexlith::domain::services
