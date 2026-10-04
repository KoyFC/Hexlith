#ifndef HEXLITH_DOMAIN_ANALYSIS_RECORD_TEXT_H
#define HEXLITH_DOMAIN_ANALYSIS_RECORD_TEXT_H

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::analysis {

// The line grammar shared by every analysis table of ADR 0006. The tables
// themselves live in tables.h; this is the layer that knows how a line looks
// and nothing about what the fields mean.
//
//     # a comment
//     format version=1
//     0x0012A4B0 size=0x000000C4 kind=func name=FieldMain source=crs-export
//
// A record is an address followed by key=value fields separated by single
// spaces. A value holds no whitespace, which the four tables can promise
// because every value they write is a number, a keyword or a symbol name.
//
// The version line is a record of the format rather than a comment because a
// tool reading a file a later version wrote has to be able to refuse it. A
// comment would be skipped and the records misread.

// Bumped when a change would make an older reader misunderstand a file.
constexpr int FormatVersion = 1;

struct RecordField {
    std::string key;
    std::string value;
};

// One parsed line, with the line number it came from so that a complaint can
// name the place a person has to go and edit.
struct Record {
    std::uint32_t address = 0;
    std::vector<RecordField> fields;
    std::size_t line = 0;

    [[nodiscard]] const std::string* Find(std::string_view key) const;
};

struct RecordFile {
    int version = FormatVersion;
    std::vector<Record> records;
};

core::Result<RecordFile> ParseRecordText(std::string_view text);

// Reads one field, failing with a message that names the line and the key.
// Missing is a failure rather than an empty value, because a table that has
// grown a field is better off saying which record lacks it than guessing.
core::Result<std::string_view> RequiredField(const Record& record, std::string_view key);

core::Result<std::uint32_t> RequiredAddressField(const Record& record, std::string_view key);

// A guest address in the canonical form every writer emits: eight uppercase
// hexadecimal digits behind 0x. Fixed width is what keeps a regenerated file
// diffable against the one before it, and it also makes the files sort the
// same way with or without a tool.
std::string RenderAddress(std::uint32_t address);

// Accepts any width from one to eight digits, in either case, so that a hand
// written 0x100000 is read as the same address the writer spells 0x00100000.
core::Result<std::uint32_t> ParseAddress(std::string_view text);

// The first line of every file, which is the format version and nothing else.
std::string RenderVersionLine();

// Joins an address and its fields into one line, terminator included.
std::string RenderRecord(std::uint32_t address, std::span<const RecordField> fields);

// Wraps each line of text in a comment marker, for the banner a writer puts at
// the head of a file. An empty line becomes a bare marker rather than a marker
// and a trailing space, so that the output has no trailing whitespace anywhere.
std::string RenderComment(std::string_view text);

}  // namespace hexlith::domain::analysis

#endif  // HEXLITH_DOMAIN_ANALYSIS_RECORD_TEXT_H
