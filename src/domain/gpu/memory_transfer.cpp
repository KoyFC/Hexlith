#include "domain/gpu/memory_transfer.h"

#include "domain/gpu/pixel_formats.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

// The bytes of GPU memory from an address to the end of the memory it is
// in. Azahar takes an address at the very end as valid, the end of an empty
// range.
std::span<std::uint8_t> Rest(GpuMemory& memory, std::uint32_t address) {
    struct Region {
        std::uint32_t base;
        std::uint32_t size;
    };

    constexpr std::array<Region, 2> Regions = {
        {{.base = GpuMemory::VramBase, .size = GpuMemory::VramSize},
         {.base = GpuMemory::FcramBase, .size = GpuMemory::FcramSize}}};
    for (const Region& region : Regions) {
        if (address >= region.base && address - region.base <= region.size) {
            const std::uint32_t remaining = region.size - (address - region.base);
            if (remaining == 0) {
                return {};
            }
            return memory.Bytes(address, remaining);
        }
    }
    return {};
}

bool InGpuMemory(std::uint32_t address) {
    return (address >= GpuMemory::VramBase &&
            address - GpuMemory::VramBase <= GpuMemory::VramSize) ||
           (address >= GpuMemory::FcramBase &&
            address - GpuMemory::FcramBase <= GpuMemory::FcramSize);
}

// GPU_MEMCOPY_FLAGS. Source: GBATEK; Azahar's DisplayTransferConfig.
namespace flag {
constexpr unsigned FlipVertically = 0;
constexpr unsigned InputLinear = 1;
constexpr unsigned CropInputLines = 2;
constexpr unsigned TextureCopy = 3;
constexpr unsigned DontSwizzle = 5;
constexpr unsigned InputFormat = 8;
constexpr unsigned OutputFormat = 12;
constexpr unsigned Scaling = 24;
}  // namespace flag

// The output downscale: 1 halves the low halfword's dimension, which GBATEK
// calls the height and Azahar the width, since the screens are on their
// side; 2 halves both; 3 is invalid.
constexpr std::uint32_t NoScale = 0;
constexpr std::uint32_t ScaleXY = 2;

constexpr std::uint32_t Low(std::uint32_t word) {
    return word & 0xFFFFU;
}

constexpr std::uint32_t High(std::uint32_t word) {
    return word >> 16U;
}

// Where pixel (x, y) of a buffer w pixels wide is, tiled or not.
std::size_t Offset(bool tiled,
                   std::uint32_t x,
                   std::uint32_t y,
                   std::uint32_t width,
                   std::size_t bytes_per_pixel) {
    return tiled ? TiledOffset(x, y, width, bytes_per_pixel)
                 : ((static_cast<std::size_t>(y) * width) + x) * bytes_per_pixel;
}

Color Read(std::span<const std::uint8_t> bytes, std::size_t offset, PixelFormat format) {
    const std::size_t size = BytesPerPixel(format);
    if (offset + size > bytes.size()) {
        return Color{};
    }
    return DecodePixel(format, bytes.subspan(offset, size));
}

// The average of pixels, a channel at a time, rounded down.
Color Average(std::span<const Color> colors) {
    std::array<std::uint32_t, 4> sums{};
    for (const Color& color : colors) {
        sums[0] += color.red;
        sums[1] += color.green;
        sums[2] += color.blue;
        sums[3] += color.alpha;
    }
    const auto count = static_cast<std::uint32_t>(colors.size());
    return Color{.red = static_cast<std::uint8_t>(sums[0] / count),
                 .green = static_cast<std::uint8_t>(sums[1] / count),
                 .blue = static_cast<std::uint8_t>(sums[2] / count),
                 .alpha = static_cast<std::uint8_t>(sums[3] / count)};
}

// Raw bytes in lines: each side reads or writes its width, then skips its
// gap, until the size is copied. A gap of zero makes a side one line.
TransferOutcome CopyTexture(GpuMemory& memory, const TransferConfig& config) {
    constexpr std::uint32_t Unit = 16;
    std::uint32_t remaining = config.texture_size / Unit * Unit;
    const std::uint32_t input_gap = High(config.texture_input) * Unit;
    const std::uint32_t output_gap = High(config.texture_output) * Unit;
    const std::uint32_t input_width = input_gap == 0 ? remaining : Low(config.texture_input) * Unit;
    const std::uint32_t output_width =
        output_gap == 0 ? remaining : Low(config.texture_output) * Unit;
    if (remaining == 0 || input_width == 0 || output_width == 0) {
        return TransferOutcome::Refused;
    }
    const std::span<std::uint8_t> input = Rest(memory, config.input);
    const std::span<std::uint8_t> output = Rest(memory, config.output);
    std::size_t in = 0;
    std::size_t out = 0;
    std::uint32_t input_left = input_width;
    std::uint32_t output_left = output_width;
    while (remaining > 0) {
        const std::uint32_t size = std::min({input_left, output_left, remaining});
        for (std::uint32_t byte = 0; byte < size; ++byte) {
            if (in + byte < input.size() && out + byte < output.size()) {
                output[out + byte] = input[in + byte];
            }
        }
        in += size;
        out += size;
        input_left -= size;
        output_left -= size;
        remaining -= size;
        if (input_left == 0) {
            input_left = input_width;
            in += input_gap;
        }
        if (output_left == 0) {
            output_left = output_width;
            out += output_gap;
        }
    }
    return TransferOutcome::Done;
}

// Pixels, converted from one format to another, from tiled to linear or the
// other way or neither, flipped and downscaled as the flags say.
TransferOutcome TransferDisplay(GpuMemory& memory, const TransferConfig& config) {
    const std::uint32_t input_width = Low(config.input_size);
    const std::uint32_t output_full_width = Low(config.output_size);
    const std::uint32_t output_full_height = High(config.output_size);
    if (input_width == 0 || High(config.input_size) == 0 || output_full_width == 0 ||
        output_full_height == 0) {
        return TransferOutcome::Refused;
    }
    const bool flip = Field(config.flags, flag::FlipVertically, 1) != 0;
    const bool input_linear = Field(config.flags, flag::InputLinear, 1) != 0;
    const bool swizzle = Field(config.flags, flag::DontSwizzle, 1) == 0;
    const std::uint32_t scaling = Field(config.flags, flag::Scaling, 2);
    const std::optional<PixelFormat> input_format =
        TransferFormat(Field(config.flags, flag::InputFormat, 3));
    const std::optional<PixelFormat> output_format =
        TransferFormat(Field(config.flags, flag::OutputFormat, 3));
    if (scaling > ScaleXY || (input_linear && scaling != NoScale) || !input_format ||
        !output_format) {
        return TransferOutcome::Unsupported;
    }
    const std::size_t input_bytes = BytesPerPixel(*input_format);
    const std::size_t output_bytes = BytesPerPixel(*output_format);

    // A flipped transfer that crops its input lines lands lower on hardware,
    // by the cropped pixels of every line but one, and games rely on it.
    // Source: Azahar.
    std::uint32_t output_address = config.output;
    if (flip && Field(config.flags, flag::CropInputLines, 1) != 0) {
        output_address += (input_width - output_full_width) * (output_full_height - 1) *
                          static_cast<std::uint32_t>(output_bytes);
    }
    const std::span<const std::uint8_t> input = Rest(memory, config.input);
    const std::span<std::uint8_t> output = Rest(memory, output_address);

    const std::uint32_t horizontal = scaling != NoScale ? 1 : 0;
    const std::uint32_t vertical = scaling == ScaleXY ? 1 : 0;
    const std::uint32_t output_width = output_full_width >> horizontal;
    const std::uint32_t output_height = output_full_height >> vertical;
    // Tiled input becomes linear output, linear input tiled output, unless
    // swizzling is off, when both stay as they are.
    const bool input_tiled = !input_linear;
    const bool output_tiled = swizzle ? input_linear : !input_linear;
    // A downscale averages the pixel and the ones after it in memory, which
    // in a tile are its neighbours. Source: Azahar.
    const std::size_t averaged = std::size_t{1} << (horizontal + vertical);
    for (std::uint32_t y = 0; y < output_height; ++y) {
        const std::uint32_t output_y = flip ? output_height - y - 1 : y;
        for (std::uint32_t x = 0; x < output_width; ++x) {
            const std::size_t source =
                Offset(input_tiled, x << horizontal, y << vertical, input_width, input_bytes);
            std::array<Color, 4> pixels{};
            for (std::size_t at = 0; at < averaged; ++at) {
                pixels.at(at) = Read(input, source + (at * input_bytes), *input_format);
            }
            const std::size_t target =
                Offset(output_tiled, x, output_y, output_width, output_bytes);
            if (target + output_bytes <= output.size()) {
                EncodePixel(*output_format,
                            Average(std::span<const Color>(pixels).first(averaged)),
                            output.subspan(target, output_bytes));
            }
        }
    }
    return TransferOutcome::Done;
}

}  // namespace

bool RunMemoryFill(GpuMemory& memory, const MemoryFillConfig& config) {
    // GPU_MEMFILL_CNT bits 8 and 9: 16 bits, 24, 32, and 3 as 24. A fill of
    // 16 or 24 bits whose range is not a whole number of values writes the
    // last one whole, past the end; one of 32 bits stops short. Source:
    // GBATEK; Azahar for the ends.
    constexpr unsigned WidthShift = 8;
    if (!InGpuMemory(config.start) || !InGpuMemory(config.end) || config.end <= config.start) {
        return false;
    }
    const std::span<std::uint8_t> bytes = Rest(memory, config.start);
    std::uint32_t length = config.end - config.start;
    const std::uint32_t width = Field(config.control, WidthShift, 2);
    std::uint32_t step = 2;
    if ((width & 1U) != 0) {
        step = 3;
    } else if (width == 2) {
        step = 4;
        length -= length % step;
    }
    for (std::uint32_t at = 0; at < length; at += step) {
        for (std::uint32_t byte = 0; byte < step && at + byte < bytes.size(); ++byte) {
            bytes[at + byte] = static_cast<std::uint8_t>(config.value >> (8 * byte));
        }
    }
    return true;
}

TransferOutcome RunTransfer(GpuMemory& memory, const TransferConfig& config) {
    if (!InGpuMemory(config.input) || !InGpuMemory(config.output)) {
        return TransferOutcome::Refused;
    }
    if (Field(config.flags, flag::TextureCopy, 1) != 0) {
        return CopyTexture(memory, config);
    }
    return TransferDisplay(memory, config);
}

}  // namespace hexlith::domain::gpu
