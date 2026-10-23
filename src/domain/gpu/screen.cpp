#include "domain/gpu/screen.h"

#include <optional>
#include <span>

namespace hexlith::domain::gpu {
namespace {

// The words of a game's description: which framebuffer it describes, the
// left and right eyes' addresses, the bytes from one row to the next, the
// format, and which framebuffer to show. Source: FrameBufferInfo in Azahar's
// gsp_gpu.h.
namespace word {
constexpr std::size_t Index = 0;
constexpr std::size_t Left = 1;
constexpr std::size_t Stride = 3;
constexpr std::size_t Format = 4;
constexpr std::size_t Shown = 5;
}  // namespace word

// The LCDs are 240 pixels high and lie on their side: a framebuffer's row
// of 240 pixels is a column of the screen, left to right, starting at the
// bottom. Source: GBATEK, "the display is drawn from left to right"; and the
// framebuffers of the reference title, which come out upright this way.
constexpr std::uint32_t Height = 240;
constexpr std::array<std::uint32_t, 2> Widths = {400, 320};

// GPU_FB_FORMAT bits 0 to 2, in the transfer engine's order; the zoom and
// the other bits do not change what is read.
constexpr std::uint32_t FormatMask = 0x7;
constexpr std::uint8_t Opaque = 0xFF;

}  // namespace

void ScreenSetup::Apply(const RecordedFramebuffer& framebuffer) {
    if (framebuffer.screen >= screens_.size()) {
        return;
    }
    Screen& screen = screens_.at(framebuffer.screen);
    const std::uint32_t index = framebuffer.words.at(word::Index) & 1U;
    screen.framebuffers.at(index).left =
        GpuMemory::PhysicalFromVirtual(framebuffer.words.at(word::Left)).value_or(0);
    screen.stride = framebuffer.words.at(word::Stride);
    screen.format = framebuffer.words.at(word::Format);
    screen.shown = framebuffer.words.at(word::Shown) & 1U;
}

Picture ScreenSetup::Compose(const GpuMemory& memory, std::uint32_t screen) const {
    Picture picture;
    if (screen >= screens_.size()) {
        return picture;
    }
    picture.width = Widths.at(screen);
    picture.height = Height;
    picture.pixels.assign(static_cast<std::size_t>(picture.width) * picture.height,
                          Color{.red = 0, .green = 0, .blue = 0, .alpha = Opaque});
    const Screen& setup = screens_.at(screen);
    const std::optional<PixelFormat> format = TransferFormat(setup.format & FormatMask);
    const std::uint32_t address = setup.framebuffers.at(setup.shown).left;
    if (!format.has_value() || address == 0) {
        return picture;
    }
    const auto bytes_per_pixel = static_cast<std::uint32_t>(BytesPerPixel(*format));
    for (std::uint32_t x = 0; x < picture.width; ++x) {
        const std::span<const std::uint8_t> row =
            memory.Bytes(address + (x * setup.stride), Height * bytes_per_pixel);
        if (row.empty()) {
            continue;
        }
        for (std::uint32_t y = 0; y < Height; ++y) {
            const std::size_t at = static_cast<std::size_t>(Height - 1 - y) * bytes_per_pixel;
            Color color = DecodePixel(*format, row.subspan(at, bytes_per_pixel));
            color.alpha = Opaque;
            picture.pixels.at((static_cast<std::size_t>(y) * picture.width) + x) = color;
        }
    }
    return picture;
}

}  // namespace hexlith::domain::gpu
