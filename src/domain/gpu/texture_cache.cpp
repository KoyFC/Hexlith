#include "domain/gpu/texture_cache.h"

#include <algorithm>

namespace hexlith::domain::gpu {
namespace {

constexpr unsigned GreenShift = 8;
constexpr unsigned BlueShift = 16;
constexpr unsigned AlphaShift = 24;

}  // namespace

std::span<const std::uint32_t> TextureCache::Texels(std::uint32_t address,
                                                    TextureFormat format,
                                                    std::uint32_t width,
                                                    std::uint32_t height,
                                                    std::span<const std::uint8_t> bytes) {
    const Key key{address, format, width, height};
    if (const auto found = entries_.find(key); found != entries_.end()) {
        Entry& entry = found->second;
        if (std::ranges::equal(entry.bytes, bytes)) {
            return entry.texels;
        }
        held_texels_ -= entry.texels.size();
        entries_.erase(found);
    }
    const std::size_t count = static_cast<std::size_t>(width) * height;
    // Everything goes when the next texture would not fit: which textures are
    // worth keeping is for a measurement to say, and the title's fit.
    if (held_texels_ + count > limit_texels_) {
        entries_.clear();
        held_texels_ = 0;
    }
    ++decoded_;
    Entry& entry = entries_[key];
    entry.bytes.assign(bytes.begin(), bytes.end());
    entry.texels.reserve(count);
    for (const Color& color : DecodeTexture(bytes, format, width, height)) {
        entry.texels.push_back(static_cast<std::uint32_t>(color.red) |
                               (static_cast<std::uint32_t>(color.green) << GreenShift) |
                               (static_cast<std::uint32_t>(color.blue) << BlueShift) |
                               (static_cast<std::uint32_t>(color.alpha) << AlphaShift));
    }
    held_texels_ += entry.texels.size();
    return entry.texels;
}

}  // namespace hexlith::domain::gpu
