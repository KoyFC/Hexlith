#ifndef HEXLITH_DOMAIN_GPU_RECORDING_INDEX_H
#define HEXLITH_DOMAIN_GPU_RECORDING_INDEX_H

#include "core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hexlith::domain::gpu {

// The events of a GPU recording's index, recording.txt, in the order they
// happened. docs/formats/gpu-recording.md is the format.

// A GX command as the game wrote it.
struct RecordedCommand {
    std::uint64_t frame = 0;
    std::array<std::uint32_t, 8> words{};
};

// A page of GPU-visible memory, at the game's virtual address, that now holds
// the content named by its SHA-256.
struct RecordedPage {
    std::uint64_t frame = 0;
    std::uint32_t address = 0;
    std::string digest;
};

// A command list's address and size as the game gave them, and the file its
// bytes are in.
struct RecordedList {
    std::uint64_t frame = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::string file;
};

// The words a game describes a framebuffer with.
constexpr std::size_t FramebufferWords = 7;

// A screen, 0 for the top and 1 for the bottom, told to show a framebuffer.
// The words are the game's description, from format 2 on.
struct RecordedFramebuffer {
    std::uint64_t frame = 0;
    std::uint32_t screen = 0;
    std::array<std::uint32_t, FramebufferWords> words{};
};

using RecordingEvent =
    std::variant<RecordedCommand, RecordedPage, RecordedList, RecordedFramebuffer>;

struct RecordingIndex {
    // 1 or 2.
    unsigned format = 0;
    std::vector<RecordingEvent> events;
};

// Reads a whole index. Refuses a format it does not know, a line it cannot
// read, and a frame that goes backwards, naming the line.
core::Result<RecordingIndex> ParseRecordingIndex(std::string_view text);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_RECORDING_INDEX_H
