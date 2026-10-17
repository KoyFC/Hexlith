#include "application/gpu_recording.h"
#include "domain/services/graphics.h"

#include "service_harness.h"

#include <format>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

namespace kernel = domain::kernel;
namespace ipc = domain::kernel::ipc;
using domain::services::Graphics;
using domain::services::testing::ServiceHarness;

// gsp::Gpu command headers, and where the first client's command queue and
// its first command are. Source: 3dbrew, GSP Services; Azahar's gsp_gpu.cpp.
constexpr std::uint32_t TriggerCmdReqQueue = 0x000C0000;
constexpr std::uint32_t RegisterInterruptRelayQueue = 0x00130042;
constexpr std::uint32_t AcquireRight = 0x00160042;
constexpr std::uint32_t CommandQueue = 0x800;
constexpr std::uint32_t FirstCommand = CommandQueue + 0x20;
constexpr std::uint32_t ListSize = 16;
constexpr std::uint8_t FirstContent = 0xAB;
constexpr std::uint8_t SecondContent = 0xCD;

// What a recording was handed, in order, one line each.
class MemoryRecorder final : public ports::GpuRecorder {
public:
    core::Status Command(std::uint64_t frame, const std::array<std::uint32_t, 8>& words) override {
        events.push_back(std::format("{} gx {:08X}", frame, words.front()));
        return {};
    }

    core::Status Page(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) override {
        events.push_back(std::format("{} page {:08X} {:02X}", frame, address, bytes.front()));
        return {};
    }

    core::Status List(std::uint64_t frame, std::uint32_t address, core::ByteSpan bytes) override {
        events.push_back(std::format("{} list {:08X} {}", frame, address, bytes.size()));
        return {};
    }

    core::Status Finish() override {
        return {};
    }

    [[nodiscard]] std::string Describe() const override {
        return "memory";
    }

    std::vector<std::string> events;
};

struct Fixture {
    Fixture() : gpu(harness.Kernel()), recording(harness.Kernel(), gpu, recorder) {
        auto event = std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
        const kernel::Handle handle = harness.Kernel().Handles().Create(event).value_or(0);
        EXPECT_TRUE(harness.Send(
            gpu, {RegisterInterruptRelayQueue, 1, ipc::CopyHandlesDescriptor(1), handle}));
        EXPECT_TRUE(harness.Send(gpu, {AcquireRight, 0, ipc::CopyHandlesDescriptor(1), 0}));
        linear = harness.Kernel()
                     .Space()
                     .CommitLinear(0, kernel::layout::PageSize, domain::memory::Access::ReadWrite)
                     .address;
    }

    void PutWord(std::uint32_t offset, std::uint32_t value) const {
        const std::span<std::uint8_t> bytes = gpu.SharedBlock()->pages.front().backing->Bytes();
        for (unsigned byte = 0; byte < 4; ++byte) {
            bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
        }
    }

    // Queues one command list at the start of the linear page and runs it.
    void SubmitList() {
        PutWord(FirstCommand, Graphics::SubmitCommandListId);
        PutWord(FirstCommand + 4, linear);
        PutWord(FirstCommand + 8, ListSize);
        PutWord(CommandQueue, 1U << 8U);
        EXPECT_TRUE(harness.Send(gpu, {TriggerCmdReqQueue}));
    }

    ServiceHarness harness;
    Graphics gpu;
    MemoryRecorder recorder;
    GpuRecording recording;
    std::uint32_t linear = 0;
};

TEST(GpuRecording, RecordsTheChangedPagesBeforeTheList) {
    Fixture fixture;
    fixture.harness.WriteBytes(fixture.linear, std::vector<std::uint8_t>{FirstContent});
    fixture.SubmitList();
    EXPECT_EQ(fixture.recorder.events,
              (std::vector<std::string>{
                  "0 gx 00000001",
                  std::format("0 page {:08X} AB", fixture.linear),
                  std::format("0 list {:08X} {}", fixture.linear, ListSize),
              }));
    EXPECT_EQ(fixture.recording.Summary().lists, 1U);
    EXPECT_EQ(fixture.recording.Summary().commands.at(1), 1U);
}

TEST(GpuRecording, RecordsAPageAgainOnlyOnceItChanges) {
    Fixture fixture;
    fixture.harness.WriteBytes(fixture.linear, std::vector<std::uint8_t>{FirstContent});
    fixture.SubmitList();
    fixture.SubmitList();
    EXPECT_EQ(fixture.recording.Summary().pages, 1U) << "the second list found nothing new";
    fixture.harness.WriteBytes(fixture.linear, std::vector<std::uint8_t>{SecondContent});
    fixture.SubmitList();
    EXPECT_EQ(fixture.recording.Summary().pages, 2U);
    EXPECT_TRUE(fixture.recording.Finish().Ok());
}

TEST(GpuRecording, LeavesOutAPageThatHoldsOnlyZeros) {
    Fixture fixture;
    fixture.SubmitList();
    EXPECT_EQ(fixture.recording.Summary().pages, 0U);
}

}  // namespace
}  // namespace hexlith::application
