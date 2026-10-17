#include "core/sha256.h"
#include "domain/services/configuration.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// cfg:u command headers. Source: 3dbrew, CFG Services.
constexpr std::uint32_t GetConfig = 0x00010082;
constexpr std::uint32_t GetRegion = 0x00020000;
constexpr std::uint32_t GetTransferableId = 0x00030040;
constexpr std::uint32_t IsCoppacsSupported = 0x00040000;
constexpr std::uint32_t GetSystemModel = 0x00050000;
constexpr std::uint32_t GetModelNintendo2DS = 0x00060000;

constexpr std::uint32_t WriteAccess = 2;
constexpr std::uint32_t Output = ServiceHarness::BufferAddress;

struct Harness {
    bool Read(std::uint32_t block, std::uint32_t size) {
        return harness.Send(
            service,
            {GetConfig, size, block, ipc::MappedBufferDescriptorFor(size, WriteAccess), Output});
    }

    ServiceHarness harness;
    Configuration service;
};

TEST(Configuration, ReadsTheLanguageBlock) {
    constexpr std::uint8_t English = 1;
    Harness cfg;
    ASSERT_TRUE(cfg.Read(Configuration::LanguageBlock, 1));
    EXPECT_EQ(cfg.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(cfg.harness.ReadBytes(Output, 1), std::vector<std::uint8_t>{English});
}

TEST(Configuration, DeclinesABlockItDoesNotHold) {
    constexpr std::uint32_t Unheld = 0x00120000;
    Harness cfg;
    EXPECT_FALSE(cfg.Read(Unheld, 4));
}

TEST(Configuration, DeclinesMoreOfABlockThanItHolds) {
    Harness cfg;
    EXPECT_FALSE(cfg.Read(Configuration::LanguageBlock, 2));
}

TEST(Configuration, AnswersTheRegionAndTheModel) {
    Harness cfg;
    ASSERT_TRUE(cfg.harness.Send(cfg.service, {GetRegion}));
    EXPECT_EQ(cfg.harness.Reply(2), Configuration::Region);
    ASSERT_TRUE(cfg.harness.Send(cfg.service, {GetSystemModel}));
    EXPECT_EQ(cfg.harness.Reply(2), Configuration::Model);
    ASSERT_TRUE(cfg.harness.Send(cfg.service, {GetModelNintendo2DS}));
    EXPECT_EQ(cfg.harness.Reply(2), 1U) << "not a 2DS";
}

TEST(Configuration, SupportsCoppacsInTheAmericas) {
    Harness cfg;
    ASSERT_TRUE(cfg.harness.Send(cfg.service, {IsCoppacsSupported}));
    EXPECT_EQ(cfg.harness.Reply(2), 1U);
}

TEST(Configuration, DerivesTheTransferableIdFromTheUniqueIdAndTheSalt) {
    // The salt the reference title sends has more than 20 bits; only those
    // count.
    constexpr std::uint32_t Salt = 0x0FF55D00;
    constexpr std::uint32_t SaltMask = 0x000FFFFF;
    Harness cfg;
    ASSERT_TRUE(cfg.harness.Send(cfg.service, {GetTransferableId, Salt}));

    std::vector<std::uint8_t> input = *cfg.service.Block(Configuration::ConsoleUniqueId2Block);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        input.push_back(static_cast<std::uint8_t>((Salt & SaltMask) >> shift));
    }
    const core::Digest digest = core::Sha256::Of(input);
    // The ID is the digest's last eight bytes, read little-endian.
    const std::size_t first = digest.bytes.size() - 8;
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        low |= static_cast<std::uint32_t>(digest.bytes.at(first + byte)) << (8U * byte);
        high |= static_cast<std::uint32_t>(digest.bytes.at(first + 4 + byte)) << (8U * byte);
    }
    EXPECT_EQ(cfg.harness.Reply(2), low);
    EXPECT_EQ(cfg.harness.Reply(3), high);
}

}  // namespace
}  // namespace hexlith::domain::services
