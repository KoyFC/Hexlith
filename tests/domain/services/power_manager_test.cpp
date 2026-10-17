#include "domain/services/power_manager.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// ptm:u's GetStepHistory. Source: 3dbrew, PTM Services.
constexpr std::uint32_t GetStepHistory = 0x000B00C2;
constexpr std::uint32_t WriteAccess = 2;

TEST(PowerManager, CountsNoStepsInAnyHour) {
    constexpr std::uint32_t Hours = 24;
    constexpr std::uint32_t Size = Hours * 2;
    constexpr std::uint8_t Filler = 0xFF;
    ServiceHarness harness;
    harness.WriteBytes(ServiceHarness::BufferAddress, std::vector<std::uint8_t>(Size, Filler));
    PowerManager ptm;
    ASSERT_TRUE(harness.Send(ptm,
                             {GetStepHistory,
                              Hours,
                              0,
                              0,
                              ipc::MappedBufferDescriptorFor(Size, WriteAccess),
                              ServiceHarness::BufferAddress}));
    EXPECT_EQ(harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(harness.ReadBytes(ServiceHarness::BufferAddress, Size),
              std::vector<std::uint8_t>(Size, 0));
}

}  // namespace
}  // namespace hexlith::domain::services
