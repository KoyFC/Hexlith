#include "domain/gpu/recording_index.h"

#include <format>
#include <limits>
#include <optional>

namespace hexlith::domain::gpu {
namespace {

constexpr std::string_view FormatPrefix = "# hexlith gpu recording, format ";
constexpr unsigned FirstFormat = 1;
constexpr unsigned LastFormat = 2;

// A SHA-256 in lower-case hexadecimal.
constexpr std::size_t DigestLength = 64;
constexpr std::uint32_t Screens = 2;

constexpr int Decimal = 10;
constexpr int Hexadecimal = 16;

// The fields of a line, split at single spaces.
std::vector<std::string_view> Fields(std::string_view line) {
    std::vector<std::string_view> fields;
    while (!line.empty()) {
        const std::size_t space = line.find(' ');
        fields.push_back(line.substr(0, space));
        if (space == std::string_view::npos) {
            break;
        }
        line.remove_prefix(space + 1);
    }
    return fields;
}

// A number in decimal or hexadecimal, every character a digit, parsed by hand
// so that the reader works over a string_view without taking its data
// pointer.
template<typename T>
std::optional<T> Number(std::string_view text, int base) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        int digit = base;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        }
        const auto wide_base = static_cast<std::uint64_t>(base);
        if (digit >= base ||
            value >
                (std::numeric_limits<T>::max() - static_cast<std::uint64_t>(digit)) / wide_base) {
            return std::nullopt;
        }
        value = (value * wide_base) + static_cast<std::uint64_t>(digit);
    }
    return static_cast<T>(value);
}

// The eight-digit hexadecimal words from a field on.
template<std::size_t Count>
std::optional<std::array<std::uint32_t, Count>> Words(const std::vector<std::string_view>& fields,
                                                      std::size_t first) {
    if (fields.size() != first + Count) {
        return std::nullopt;
    }
    std::array<std::uint32_t, Count> words{};
    for (std::size_t index = 0; index < Count; ++index) {
        const std::optional<std::uint32_t> word =
            Number<std::uint32_t>(fields[first + index], Hexadecimal);
        if (!word.has_value()) {
            return std::nullopt;
        }
        words.at(index) = *word;
    }
    return words;
}

bool IsDigest(std::string_view text) {
    return text.size() == DigestLength &&
           text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::optional<RecordingEvent>
ParseEvent(std::uint64_t frame, const std::vector<std::string_view>& fields, unsigned format) {
    const std::string_view kind = fields.at(1);
    if (kind == "gx") {
        const auto words = Words<8>(fields, 2);
        return words
                   ? std::optional<RecordingEvent>{RecordedCommand{.frame = frame, .words = *words}}
                   : std::nullopt;
    }
    if (kind == "page" && fields.size() == 4 && IsDigest(fields[3])) {
        const auto address = Number<std::uint32_t>(fields[2], Hexadecimal);
        if (address.has_value()) {
            return RecordedPage{
                .frame = frame, .address = *address, .digest = std::string{fields[3]}};
        }
    }
    if (kind == "list" && fields.size() == 5) {
        const auto address = Number<std::uint32_t>(fields[2], Hexadecimal);
        const auto size = Number<std::uint32_t>(fields[3], Hexadecimal);
        if (address.has_value() && size.has_value() && !fields[4].empty() &&
            fields[4].find('/') == std::string_view::npos) {
            return RecordedList{
                .frame = frame, .address = *address, .size = *size, .file = std::string{fields[4]}};
        }
    }
    if (kind == "framebuffer" && format >= 2 && fields.size() > 2) {
        const auto screen = Number<std::uint32_t>(fields[2], Decimal);
        const auto words = Words<FramebufferWords>(fields, 3);
        if (screen.has_value() && *screen < Screens && words.has_value()) {
            return RecordedFramebuffer{.frame = frame, .screen = *screen, .words = *words};
        }
    }
    return std::nullopt;
}

}  // namespace

core::Result<RecordingIndex> ParseRecordingIndex(std::string_view text) {
    RecordingIndex index;
    std::size_t number = 0;
    std::uint64_t last_frame = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++number;

        if (number == 1) {
            const auto format = line.starts_with(FormatPrefix)
                                    ? Number<unsigned>(line.substr(FormatPrefix.size()), Decimal)
                                    : std::nullopt;
            if (!format.has_value() || *format < FirstFormat || *format > LastFormat) {
                return core::Error{"not a GPU recording of a format this reads"};
            }
            index.format = *format;
            continue;
        }
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string_view> fields = Fields(line);
        const auto frame = Number<std::uint64_t>(fields.front(), Decimal);
        const std::optional<RecordingEvent> event = frame.has_value() && fields.size() > 1
                                                        ? ParseEvent(*frame, fields, index.format)
                                                        : std::nullopt;
        if (!event.has_value()) {
            return core::Error{std::format("line {} cannot be read: '{}'", number, line)};
        }
        if (*frame < last_frame) {
            return core::Error{std::format("line {} goes back to frame {}", number, *frame)};
        }
        last_frame = *frame;
        index.events.push_back(*event);
    }
    if (number == 0) {
        return core::Error{"the index is empty"};
    }
    return index;
}

}  // namespace hexlith::domain::gpu
