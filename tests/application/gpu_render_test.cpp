#include "application/gpu_render.h"

#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

// A recording that is only its index: no pages and no lists.
class IndexOnlySource final : public ports::GpuRecordingSource {
public:
    explicit IndexOnlySource(std::string index) : index_(std::move(index)) {}

    core::Result<std::string> Index() override {
        return index_;
    }

    core::Result<std::vector<std::uint8_t>> Page(std::string_view /*digest*/) override {
        return core::Error{"no pages"};
    }

    core::Result<std::vector<std::uint8_t>> List(std::string_view /*file*/) override {
        return core::Error{"no lists"};
    }

    [[nodiscard]] std::string Describe() const override {
        return "index";
    }

private:
    std::string index_;
};

// The bottom screen shows an RGB8 framebuffer at the start of VRAM from frame
// 1; a memory fill in frame 1 whitens its first row, which is the screen's
// first column; frame 3 has a cache flush and nothing else.
constexpr std::string_view Recording =
    "# hexlith gpu recording, format 2\n"
    "1 framebuffer 1 00000000 1F000000 00000000 000002D0 00000301 00000000 00000000\n"
    "1 gx 00000002 1F000000 FFFFFFFF 1F0002D0 00000000 00000000 00000000 00000201\n"
    "3 gx 00000005 00000000 00000000 00000000 00000000 00000000 00000000 00000000\n";

std::vector<RenderedFrame> Render(const std::set<std::uint64_t>& frames) {
    IndexOnlySource source{std::string{Recording}};
    auto memory = std::make_unique<gpu::GpuMemory>();
    ReferenceGpuBackend backend(*memory, gpu::Sampling::Hardware);
    std::vector<RenderedFrame> rendered;
    const core::Result<GpuRenderSummary> summary = RenderGpuRecording(
        source, *memory, backend, frames, [&rendered](const RenderedFrame& frame) {
            rendered.push_back(frame);
            return core::Status{};
        });
    EXPECT_TRUE(summary.Ok());
    return rendered;
}

// Records what the renderer tells a backend about memory, and draws nothing.
class Noting final : public ports::GpuBackend {
public:
    core::Status RunCommandList(std::span<const std::uint8_t> /*list*/) override {
        return {};
    }

    core::Status BeforeRead(std::uint32_t physical, std::uint32_t size) override {
        calls.push_back(std::format("read {:08X} {:X}", physical, size));
        return {};
    }

    core::Status BeforeWrite(std::uint32_t physical, std::uint32_t size) override {
        calls.push_back(std::format("write {:08X} {:X}", physical, size));
        return {};
    }

    [[nodiscard]] ports::GpuBackendStatistics Statistics() const override {
        return {};
    }

    [[nodiscard]] std::string Describe() const override {
        return "noting";
    }

    std::vector<std::string> calls;
};

TEST(RenderGpuRecording, TellsTheBackendWhatAFrameShowsAndAFillWrites) {
    IndexOnlySource source{std::string{Recording}};
    gpu::GpuMemory memory;
    Noting backend;
    const auto taken = [](const RenderedFrame& /*frame*/) { return core::Status{}; };
    ASSERT_TRUE(RenderGpuRecording(source, memory, backend, {1}, taken).Ok());
    // The picture of frame 1 is taken before the fill of frame 1 runs.
    EXPECT_EQ(backend.calls,
              (std::vector<std::string>{"read 18000000 38400", "write 18000000 2D0"}));
}

// The pixel the framebuffer's first row starts at: the bottom left.
std::uint8_t BottomLeftRed(const RenderedFrame& frame) {
    const std::size_t bottom_row = frame.bottom.height - 1;
    return frame.bottom.pixels.at(bottom_row * frame.bottom.width).red;
}

TEST(RenderGpuRecording, TakesAFrameBeforeItsOwnWorkAndAfterItsFramebuffers) {
    const std::vector<RenderedFrame> rendered = Render({1, 2, 3});
    ASSERT_EQ(rendered.size(), 3U);
    EXPECT_EQ(rendered[0].frame, 1U);
    EXPECT_EQ(BottomLeftRed(rendered[0]), 0) << "the fill comes after frame 1 begins";
    EXPECT_EQ(BottomLeftRed(rendered[1]), 0xFF);
    EXPECT_EQ(BottomLeftRed(rendered[2]), 0xFF);
    EXPECT_EQ(rendered[2].top.width, 400U);
}

TEST(RenderGpuRecording, HandsOnNoFramePastTheRecording) {
    const std::vector<RenderedFrame> rendered = Render({3, 9});
    ASSERT_EQ(rendered.size(), 1U);
    EXPECT_EQ(rendered[0].frame, 3U);
}

}  // namespace
}  // namespace hexlith::application
