#ifndef HEXLITH_PORTS_BLOB_SINK_H
#define HEXLITH_PORTS_BLOB_SINK_H

#include "core/byte_reader.h"
#include "core/result.h"

#include <string>
#include <string_view>

namespace hexlith::ports {

// Where extracted content is written.
//
// Paths are relative and use forward slashes. The sink owns the root they are
// relative to, which is how the use case ends up unable to write anywhere
// else: it never sees an absolute path and so cannot construct one.
class BlobSink {
public:
    BlobSink() = default;
    virtual ~BlobSink() = default;

    BlobSink(const BlobSink&) = delete;
    BlobSink& operator=(const BlobSink&) = delete;
    BlobSink(BlobSink&&) = delete;
    BlobSink& operator=(BlobSink&&) = delete;

    virtual core::Status Write(std::string_view path, core::ByteSpan data) = 0;

    virtual core::Status WriteText(std::string_view path, std::string_view text) = 0;

    // For a directory the image declares with nothing in it, which writing
    // files alone would lose.
    virtual core::Status MakeDirectory(std::string_view path) = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

}  // namespace hexlith::ports

#endif  // HEXLITH_PORTS_BLOB_SINK_H
