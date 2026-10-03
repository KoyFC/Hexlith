#ifndef HEXLITH_DOMAIN_CTR_BACKWARDS_LZSS_H
#define HEXLITH_DOMAIN_CTR_BACKWARDS_LZSS_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hexlith::domain::ctr {

// What the footer of a compressed section declares about itself.
struct BackwardsLzssFooter {
    std::size_t footer_size = 0;
    std::size_t encoded_size = 0;
    std::size_t additional_size = 0;
    std::size_t decompressed_size = 0;
};

core::Result<BackwardsLzssFooter> ReadBackwardsLzssFooter(core::ByteSpan compressed);

// Decompresses an ExeFS .code section.
//
// 3dbrew names the algorithm reverse-LZSS and gives no further detail, so the
// authoritative description is Azahar's implementation in
// src/core/file_sys/ncch_container.cpp, which docs/formats/exefs.md writes
// out field by field.
//
// The encoding is designed to be decompressed in place, which is why it runs
// backwards and why the output buffer starts as a copy of the input. Two
// consequences are worth knowing before reading the code.
//
// The bottom of the output is not encoded at all. It is whatever the input
// held there, left in place by that initial copy, and the encoded region
// covers only the top. A stream therefore cannot be larger than what it
// decompresses to, which also means an all-literal stream is impossible.
//
// Decoding stops when the read cursor reaches the start of the encoded
// region, not when the output is full. An input that stops early leaves the
// verbatim bottom untouched rather than failing.
core::Result<std::vector<std::uint8_t>> DecompressBackwardsLzss(core::ByteSpan compressed);

}  // namespace hexlith::domain::ctr

#endif  // HEXLITH_DOMAIN_CTR_BACKWARDS_LZSS_H
