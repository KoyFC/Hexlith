#include "domain/analysis/record_text.h"

#include <format>
#include <optional>

namespace hexlith::domain::analysis {
namespace {

constexpr std::string_view Whitespace = " \t\r\f\v";
constexpr char CommentMarker = '#';
constexpr char FieldSeparator = '=';
constexpr std::string_view VersionKeyword = "format";
constexpr std::string_view VersionKey = "version";
constexpr std::string_view HexPrefix = "0x";
constexpr std::string_view UpperHexPrefix = "0X";
constexpr std::size_t AddressDigits = 8;
constexpr std::uint32_t BitsPerHexDigit = 4;

// The value of one hexadecimal digit, or nothing when the character is not one.
// Written out rather than taken from a locale-aware function, which would make
// the answer depend on the environment the tool runs in.
std::optional<std::uint32_t> HexDigitValue(char digit) {
    constexpr std::uint32_t FirstLetterValue = 0xA;

    if (digit >= '0' && digit <= '9') {
        return static_cast<std::uint32_t>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return FirstLetterValue + static_cast<std::uint32_t>(digit - 'a');
    }
    if (digit >= 'A' && digit <= 'F') {
        return FirstLetterValue + static_cast<std::uint32_t>(digit - 'A');
    }
    return std::nullopt;
}

std::string_view Trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(Whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(Whitespace);
    return text.substr(first, last - first + 1);
}

std::vector<std::string_view> SplitOnWhitespace(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t position = 0;

    while (position < line.size()) {
        const std::size_t start = line.find_first_not_of(Whitespace, position);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = line.find_first_of(Whitespace, start);
        tokens.push_back(line.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start));
        position = end == std::string_view::npos ? line.size() : end;
    }

    return tokens;
}

const RecordField* FindIn(const std::vector<RecordField>& fields, std::string_view key) {
    for (const RecordField& field : fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

core::Result<std::vector<RecordField>> ParseFields(std::span<const std::string_view> tokens,
                                                   std::size_t line_number) {
    std::vector<RecordField> fields;
    fields.reserve(tokens.size());

    for (const std::string_view token : tokens) {
        const std::size_t separator = token.find(FieldSeparator);
        if (separator == std::string_view::npos) {
            return core::Error{
                std::format("line {}: '{}' is not a key=value field", line_number, token)};
        }

        const std::string_view key = token.substr(0, separator);
        const std::string_view value = token.substr(separator + 1);

        if (key.empty()) {
            return core::Error{std::format("line {}: '{}' has no key", line_number, token)};
        }
        if (value.empty()) {
            return core::Error{std::format("line {}: '{}=' has no value", line_number, key)};
        }
        if (FindIn(fields, key) != nullptr) {
            return core::Error{
                std::format("line {}: '{}' is set twice on the same record", line_number, key)};
        }

        fields.push_back(RecordField{.key = std::string{key}, .value = std::string{value}});
    }

    return fields;
}

// A decimal count, parsed by hand rather than through from_chars so that the
// reader works over a string_view without taking its data pointer. The limit is
// there to keep a file of nines from overflowing; a format version never comes
// near it.
std::optional<int> ParseDecimal(std::string_view text) {
    constexpr int Base = 10;
    constexpr int Limit = 1000000;

    if (text.empty()) {
        return std::nullopt;
    }

    int value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = (value * Base) + (digit - '0');
        if (value > Limit) {
            return std::nullopt;
        }
    }
    return value;
}

core::Result<int> ParseVersionLine(std::span<const std::string_view> tokens,
                                   std::size_t line_number) {
    const core::Result<std::vector<RecordField>> fields = ParseFields(tokens, line_number);
    if (!fields) {
        return fields.Failure();
    }

    if (fields.Value().size() != 1 || fields.Value().front().key != VersionKey) {
        return core::Error{
            std::format("line {}: the format line reads 'format version=<number>' and nothing else",
                        line_number)};
    }

    const std::string& text = fields.Value().front().value;
    const std::optional<int> version = ParseDecimal(text);
    if (!version.has_value() || version.value() == 0) {
        return core::Error{std::format("line {}: '{}' is not a format version", line_number, text)};
    }

    if (version.value() != FormatVersion) {
        return core::Error{std::format(
            "line {}: this is a version {} file and this tool reads version {}; it was written by "
            "a different version of Hexlith",
            line_number,
            version.value(),
            FormatVersion)};
    }

    return version.value();
}

}  // namespace

const std::string* Record::Find(std::string_view key) const {
    const RecordField* field = FindIn(fields, key);
    return field == nullptr ? nullptr : &field->value;
}

core::Result<RecordFile> ParseRecordText(std::string_view text) {
    RecordFile file;
    bool version_seen = false;
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

        const std::vector<std::string_view> tokens = SplitOnWhitespace(line);
        const std::span<const std::string_view> rest{tokens.begin() + 1, tokens.end()};

        if (tokens.front() == VersionKeyword) {
            if (version_seen) {
                return core::Error{
                    std::format("line {}: the format version is declared twice", line_number)};
            }
            const core::Result<int> version = ParseVersionLine(rest, line_number);
            if (!version) {
                return version.Failure();
            }
            file.version = version.Value();
            version_seen = true;
            continue;
        }

        if (!version_seen) {
            return core::Error{
                std::format("line {}: a record appears before the 'format version={}' line",
                            line_number,
                            FormatVersion)};
        }

        const core::Result<std::uint32_t> address = ParseAddress(tokens.front());
        if (!address) {
            return address.Failure().WithContext(std::format("line {}", line_number));
        }

        const core::Result<std::vector<RecordField>> fields = ParseFields(rest, line_number);
        if (!fields) {
            return fields.Failure();
        }

        file.records.push_back(
            Record{.address = address.Value(), .fields = fields.Value(), .line = line_number});
    }

    if (!version_seen) {
        return core::Error{
            std::format("the file does not declare 'format version={}'", FormatVersion)};
    }

    return file;
}

core::Result<std::string_view> RequiredField(const Record& record, std::string_view key) {
    const std::string* value = record.Find(key);
    if (value == nullptr) {
        return core::Error{std::format("line {}: the record does not set '{}'", record.line, key)};
    }
    return std::string_view{*value};
}

core::Result<std::uint32_t> RequiredAddressField(const Record& record, std::string_view key) {
    const core::Result<std::string_view> value = RequiredField(record, key);
    if (!value) {
        return value.Failure();
    }

    const core::Result<std::uint32_t> number = ParseAddress(value.Value());
    if (!number) {
        return number.Failure().WithContext(std::format("line {}: '{}'", record.line, key));
    }
    return number.Value();
}

std::string RenderAddress(std::uint32_t address) {
    return std::format("{}{:08X}", HexPrefix, address);
}

core::Result<std::uint32_t> ParseAddress(std::string_view text) {
    if (!text.starts_with(HexPrefix) && !text.starts_with(UpperHexPrefix)) {
        return core::Error{std::format("'{}' is not an address; one is written 0x followed by up "
                                       "to {} hexadecimal digits",
                                       text,
                                       AddressDigits)};
    }

    const std::string_view digits = text.substr(HexPrefix.size());
    if (digits.empty() || digits.size() > AddressDigits) {
        return core::Error{
            std::format("'{}' has {} hexadecimal digits, and an address has one to {}",
                        text,
                        digits.size(),
                        AddressDigits)};
    }

    std::uint32_t value = 0;
    for (const char digit : digits) {
        const std::optional<std::uint32_t> nibble = HexDigitValue(digit);
        if (!nibble.has_value()) {
            return core::Error{std::format("'{}' is not a hexadecimal number", text)};
        }
        value = (value << BitsPerHexDigit) | nibble.value();
    }

    return value;
}

std::string RenderVersionLine() {
    return std::format("{} {}={}\n", VersionKeyword, VersionKey, FormatVersion);
}

std::string RenderRecord(std::uint32_t address, std::span<const RecordField> fields) {
    std::string line = RenderAddress(address);
    for (const RecordField& field : fields) {
        line.push_back(' ');
        line.append(field.key);
        line.push_back(FieldSeparator);
        line.append(field.value);
    }
    line.push_back('\n');
    return line;
}

std::string RenderComment(std::string_view text) {
    std::string out;
    std::size_t position = 0;

    // A caller writing a banner ends it with a newline or does not, and either
    // way the banner is the same lines. Dropping one here keeps the second
    // spelling from growing a bare marker at the end.
    if (text.ends_with('\n')) {
        text.remove_suffix(1);
    }

    while (position <= text.size()) {
        const std::size_t break_at = text.find('\n', position);
        const std::string_view line = text.substr(
            position,
            break_at == std::string_view::npos ? std::string_view::npos : break_at - position);
        position = break_at == std::string_view::npos ? text.size() + 1 : break_at + 1;

        out.push_back(CommentMarker);
        if (!line.empty()) {
            out.push_back(' ');
            out.append(line);
        }
        out.push_back('\n');
    }

    return out;
}

}  // namespace hexlith::domain::analysis
