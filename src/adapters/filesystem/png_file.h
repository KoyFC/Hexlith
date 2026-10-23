#ifndef HEXLITH_ADAPTERS_FILESYSTEM_PNG_FILE_H
#define HEXLITH_ADAPTERS_FILESYSTEM_PNG_FILE_H

#include "core/result.h"
#include "domain/gpu/screen.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hexlith::adapters::filesystem {

// A picture as a PNG: eight-bit RGB, no interlace, the image data in
// uncompressed deflate blocks, so that nothing but the standard is needed to
// write it. Source: the PNG specification (ISO/IEC 15948) for the chunks and
// the CRC; RFC 1950 and 1951 for the zlib stream and its stored blocks.
std::vector<std::uint8_t> EncodePng(const domain::gpu::Picture& picture);

core::Status WritePng(const std::filesystem::path& path, const domain::gpu::Picture& picture);

}  // namespace hexlith::adapters::filesystem

#endif  // HEXLITH_ADAPTERS_FILESYSTEM_PNG_FILE_H
