#include "core/key_value_text.h"

#include <format>

namespace hexlith::core {
namespace {

constexpr std::string_view Whitespace = " \t\r\f\v";
constexpr char CommentMarker = '#';
constexpr char Separator = '=';

std::string_view Trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(Whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(Whitespace);
    return text.substr(first, last - first + 1);
}

}  // namespace

Result<std::vector<KeyValue>> ParseKeyValueText(std::string_view text) {
    std::vector<KeyValue> entries;
    std::size_t line_number = 0;
    std::size_t position = 0;

    while (position <= text.size()) {
        const std::size_t break_at = text.find('\n', position);
        const std::string_view raw_line = text.substr(
            position,
            break_at == std::string_view::npos ? std::string_view::npos : break_at - position);
        position = break_at == std::string_view::npos ? text.size() + 1 : break_at + 1;
        ++line_number;

        const std::string_view line = Trim(raw_line);
        if (line.empty() || line.front() == CommentMarker) {
            continue;
        }

        const std::size_t separator = line.find(Separator);
        if (separator == std::string_view::npos) {
            return Error{std::format("line {}: '{}' is not a key = value line", line_number, line)};
        }

        const std::string_view key = Trim(line.substr(0, separator));
        if (key.empty()) {
            return Error{std::format("line {}: the key is missing", line_number)};
        }

        if (const KeyValue* existing = Find(entries, key); existing != nullptr) {
            return Error{std::format(
                "line {}: '{}' was already set on line {}", line_number, key, existing->line)};
        }

        entries.push_back(KeyValue{.key = std::string{key},
                                   .value = std::string{Trim(line.substr(separator + 1))},
                                   .line = line_number});
    }

    return entries;
}

const KeyValue* Find(const std::vector<KeyValue>& entries, std::string_view name) {
    for (const KeyValue& entry : entries) {
        if (entry.key == name) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace hexlith::core
