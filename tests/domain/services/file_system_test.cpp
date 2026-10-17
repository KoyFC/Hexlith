#include "domain/services/file_system.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <numeric>
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

// Path types and the binary RomFS path of SelfNCCH. Source: 3dbrew,
// Filesystem services, Paths.
constexpr std::uint32_t EmptyPath = 1;
constexpr std::uint32_t BinaryPath = 2;
constexpr std::uint32_t RomFsPathSize = 12;
constexpr std::uint32_t ReadMode = 1;
constexpr std::uint32_t WriteAccess = 2;

// An archive ID nothing here holds: the shared extra data archive.
constexpr std::uint32_t SharedExtraData = 0x00000007;

constexpr std::uint32_t PathAddress = ServiceHarness::BufferAddress;
constexpr std::uint32_t DataAddress = ServiceHarness::BufferAddress + 0x100;

std::vector<std::uint8_t> CountingBytes(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

struct Harness {
    Harness() : romfs(CountingBytes(0x40)), service(romfs) {
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

    ServiceHarness harness;
    std::vector<std::uint8_t> romfs;
    FileSystem service;
};

TEST(FileSystem, OpensTheRomFsDirectlyAndReadsIt) {
    Harness fs;
    const auto file = fs.OpenRomFs();
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->handler->Name(), "fs:USER file");

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

}  // namespace
}  // namespace hexlith::domain::services
