#include "domain/analysis/tables.h"

#include "domain/analysis/record_text.h"

#include <algorithm>
#include <format>
#include <span>
#include <tuple>

namespace hexlith::domain::analysis {
namespace {

constexpr std::string_view SizeKey = "size";
constexpr std::string_view KindKey = "kind";
constexpr std::string_view NameKey = "name";
constexpr std::string_view SourceKey = "source";
constexpr std::string_view InstructionSetKey = "isa";
constexpr std::string_view AlternateEntriesKey = "alt-entries";
constexpr std::string_view TargetKey = "target";
constexpr char ListSeparator = ',';

constexpr std::string_view KnownSources =
    "crs-export, exheader, relocation, ghidra, heuristic or manual";

core::Result<Provenance> ReadSource(const Record& record) {
    const core::Result<std::string_view> value = RequiredField(record, SourceKey);
    if (!value) {
        return value.Failure();
    }

    const std::optional<Provenance> source = ParseProvenance(value.Value());
    if (!source.has_value()) {
        return core::Error{std::format("line {}: '{}' is not a source; it reads {}",
                                       record.line,
                                       value.Value(),
                                       KnownSources)};
    }
    return source.value();
}

core::Result<arm::InstructionSet> ReadInstructionSet(const Record& record, std::string_view value) {
    const std::optional<arm::InstructionSet> set = arm::ParseInstructionSet(value);
    if (!set.has_value()) {
        return core::Error{std::format(
            "line {}: '{}' is not an instruction set; it reads arm or thumb", record.line, value)};
    }
    return set.value();
}

core::Result<std::vector<std::uint32_t>> ReadAddressList(const Record& record,
                                                         std::string_view text) {
    std::vector<std::uint32_t> addresses;
    std::size_t position = 0;

    while (position <= text.size()) {
        const std::size_t break_at = text.find(ListSeparator, position);
        const std::string_view item = text.substr(
            position,
            break_at == std::string_view::npos ? std::string_view::npos : break_at - position);
        position = break_at == std::string_view::npos ? text.size() + 1 : break_at + 1;

        const core::Result<std::uint32_t> address = ParseAddress(item);
        if (!address) {
            return address.Failure().WithContext(
                std::format("line {}: '{}'", record.line, AlternateEntriesKey));
        }
        if (std::ranges::find(addresses, address.Value()) != addresses.end()) {
            return core::Error{std::format("line {}: '{}' lists {} twice",
                                           record.line,
                                           AlternateEntriesKey,
                                           RenderAddress(address.Value()))};
        }
        addresses.push_back(address.Value());
    }

    return addresses;
}

std::string RenderAddressList(std::span<const std::uint32_t> addresses) {
    std::string text;
    for (const std::uint32_t address : addresses) {
        if (!text.empty()) {
            text.push_back(ListSeparator);
        }
        text.append(RenderAddress(address));
    }
    return text;
}

template<typename T, typename FromRecord>
core::Result<std::vector<T>> ParseTable(std::string_view text, FromRecord from_record) {
    const core::Result<RecordFile> file = ParseRecordText(text);
    if (!file) {
        return file.Failure();
    }

    std::vector<T> records;
    records.reserve(file.Value().records.size());

    for (const Record& record : file.Value().records) {
        const core::Result<T> parsed = from_record(record);
        if (!parsed) {
            return parsed.Failure();
        }
        records.push_back(parsed.Value());
    }

    return records;
}

template<typename T, typename Less, typename ToFields>
std::string
RenderTable(std::string_view banner, std::vector<T> records, Less less, ToFields to_fields) {
    std::ranges::sort(records, less);

    std::string text = RenderComment(banner);
    text.append(RenderVersionLine());
    for (const T& record : records) {
        text.append(RenderRecord(record.address, to_fields(record)));
    }
    return text;
}

constexpr std::string_view CommonBanner =
    "Written by hexlith-analyze and edited by hand. One record per line: the\n"
    "address, then key=value fields. Lines are sorted by address and every\n"
    "number is fixed-width hexadecimal, so regenerating this file changes only\n"
    "the records that changed.\n"
    "\n"
    "source= says where a record came from. A regeneration pass replaces what\n"
    "it can derive from the cartridge again and leaves ghidra and manual alone.\n";

std::string Banner(std::string_view what) {
    return std::format("{}\n{}", what, CommonBanner);
}

}  // namespace

std::string_view Describe(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Function:
        return "func";
    case SymbolKind::Data:
        return "data";
    case SymbolKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::optional<SymbolKind> ParseSymbolKind(std::string_view text) {
    if (text == "func") {
        return SymbolKind::Function;
    }
    if (text == "data") {
        return SymbolKind::Data;
    }
    if (text == "unknown") {
        return SymbolKind::Unknown;
    }
    return std::nullopt;
}

std::string_view Describe(RegionKind kind) {
    switch (kind) {
    case RegionKind::Code:
        return "code";
    case RegionKind::Data:
        return "data";
    case RegionKind::JumpTable:
        return "jump-table";
    case RegionKind::Padding:
        return "padding";
    }
    return "unknown";
}

std::optional<RegionKind> ParseRegionKind(std::string_view text) {
    if (text == "code") {
        return RegionKind::Code;
    }
    if (text == "data") {
        return RegionKind::Data;
    }
    if (text == "jump-table") {
        return RegionKind::JumpTable;
    }
    if (text == "padding") {
        return RegionKind::Padding;
    }
    return std::nullopt;
}

core::Result<std::vector<Symbol>> ParseSymbols(std::string_view text) {
    return ParseTable<Symbol>(text, [](const Record& record) -> core::Result<Symbol> {
        Symbol symbol;
        symbol.address = record.address;

        const core::Result<std::uint32_t> size = RequiredAddressField(record, SizeKey);
        if (!size) {
            return size.Failure();
        }
        symbol.size = size.Value();

        const core::Result<std::string_view> kind = RequiredField(record, KindKey);
        if (!kind) {
            return kind.Failure();
        }
        const std::optional<SymbolKind> parsed_kind = ParseSymbolKind(kind.Value());
        if (!parsed_kind.has_value()) {
            return core::Error{
                std::format("line {}: '{}' is not a symbol kind; it reads func, data or unknown",
                            record.line,
                            kind.Value())};
        }
        symbol.kind = parsed_kind.value();

        const core::Result<std::string_view> name = RequiredField(record, NameKey);
        if (!name) {
            return name.Failure();
        }
        symbol.name = std::string{name.Value()};

        const core::Result<Provenance> source = ReadSource(record);
        if (!source) {
            return source.Failure();
        }
        symbol.source = source.Value();

        return symbol;
    });
}

std::string RenderSymbols(std::vector<Symbol> records) {
    const auto less = [](const Symbol& left, const Symbol& right) {
        return std::tie(left.address, left.name, left.kind, left.size, left.source) <
               std::tie(right.address, right.name, right.kind, right.size, right.source);
    };

    const auto to_fields = [](const Symbol& symbol) {
        return std::vector<RecordField>{
            {.key = std::string{SizeKey}, .value = RenderAddress(symbol.size)},
            {.key = std::string{KindKey}, .value = std::string{Describe(symbol.kind)}},
            {.key = std::string{NameKey}, .value = symbol.name},
            {.key = std::string{SourceKey}, .value = std::string{Describe(symbol.source)}},
        };
    };

    // Two symbols may share an address: an alias, or a name the export table
    // gives to the first instruction of a function. Sorting by name as well
    // keeps the order of the pair from depending on which pass found it first.
    return RenderTable(Banner("Symbols: an address, how far it runs, what it is and its name."),
                       std::move(records),
                       less,
                       to_fields);
}

core::Result<std::vector<FunctionRange>> ParseFunctions(std::string_view text) {
    return ParseTable<FunctionRange>(text, [](const Record& record) -> core::Result<FunctionRange> {
        FunctionRange function;
        function.address = record.address;

        const core::Result<std::uint32_t> size = RequiredAddressField(record, SizeKey);
        if (!size) {
            return size.Failure();
        }
        function.size = size.Value();

        const core::Result<std::string_view> set = RequiredField(record, InstructionSetKey);
        if (!set) {
            return set.Failure();
        }
        const core::Result<arm::InstructionSet> parsed_set =
            ReadInstructionSet(record, set.Value());
        if (!parsed_set) {
            return parsed_set.Failure();
        }
        function.instruction_set = parsed_set.Value();

        if (const std::string* entries = record.Find(AlternateEntriesKey); entries != nullptr) {
            const core::Result<std::vector<std::uint32_t>> parsed =
                ReadAddressList(record, *entries);
            if (!parsed) {
                return parsed.Failure();
            }
            function.alternate_entries = parsed.Value();
        }

        for (const std::uint32_t entry : function.alternate_entries) {
            if (entry == function.address) {
                return core::Error{std::format("line {}: {} is the function's own entry point",
                                               record.line,
                                               RenderAddress(entry))};
            }
            const bool extent_known = function.size != UnknownSize;
            const bool inside =
                entry > function.address && entry - function.address < function.size;
            if (extent_known && !inside) {
                return core::Error{
                    std::format("line {}: {} is outside the function, which runs from {} for {:#x} "
                                "bytes",
                                record.line,
                                RenderAddress(entry),
                                RenderAddress(function.address),
                                function.size)};
            }
        }

        const core::Result<Provenance> source = ReadSource(record);
        if (!source) {
            return source.Failure();
        }
        function.source = source.Value();

        return function;
    });
}

std::string RenderFunctions(std::vector<FunctionRange> records) {
    for (FunctionRange& function : records) {
        std::ranges::sort(function.alternate_entries);
    }

    const auto less = [](const FunctionRange& left, const FunctionRange& right) {
        return std::tie(left.address, left.size, left.instruction_set, left.source) <
               std::tie(right.address, right.size, right.instruction_set, right.source);
    };

    const auto to_fields = [](const FunctionRange& function) {
        std::vector<RecordField> fields{
            {.key = std::string{SizeKey}, .value = RenderAddress(function.size)},
            {.key = std::string{InstructionSetKey},
             .value = std::string{arm::Describe(function.instruction_set)}},
        };
        if (!function.alternate_entries.empty()) {
            fields.push_back({.key = std::string{AlternateEntriesKey},
                              .value = RenderAddressList(function.alternate_entries)});
        }
        fields.push_back(
            {.key = std::string{SourceKey}, .value = std::string{Describe(function.source)}});
        return fields;
    };

    return RenderTable(
        Banner("Functions: where each one starts, how far it runs, which instruction set\n"
               "it is written in, and the other addresses the lifter may enter it at."),
        std::move(records),
        less,
        to_fields);
}

core::Result<std::vector<Region>> ParseRegions(std::string_view text) {
    return ParseTable<Region>(text, [](const Record& record) -> core::Result<Region> {
        Region region;
        region.address = record.address;

        const core::Result<std::uint32_t> size = RequiredAddressField(record, SizeKey);
        if (!size) {
            return size.Failure();
        }
        region.size = size.Value();

        const core::Result<std::string_view> kind = RequiredField(record, KindKey);
        if (!kind) {
            return kind.Failure();
        }
        const std::optional<RegionKind> parsed_kind = ParseRegionKind(kind.Value());
        if (!parsed_kind.has_value()) {
            return core::Error{std::format(
                "line {}: '{}' is not a region kind; it reads code, data, jump-table or padding",
                record.line,
                kind.Value())};
        }
        region.kind = parsed_kind.value();

        const std::string* set = record.Find(InstructionSetKey);
        if (region.kind == RegionKind::Code) {
            if (set == nullptr) {
                return core::Error{
                    std::format("line {}: a code region has to say which instruction set it is in",
                                record.line)};
            }
            const core::Result<arm::InstructionSet> parsed_set = ReadInstructionSet(record, *set);
            if (!parsed_set) {
                return parsed_set.Failure();
            }
            region.instruction_set = parsed_set.Value();
        } else if (set != nullptr) {
            return core::Error{std::format(
                "line {}: a {} region has no instruction set", record.line, Describe(region.kind))};
        }

        const core::Result<Provenance> source = ReadSource(record);
        if (!source) {
            return source.Failure();
        }
        region.source = source.Value();

        return region;
    });
}

std::string RenderRegions(std::vector<Region> records) {
    const auto less = [](const Region& left, const Region& right) {
        return std::tie(left.address, left.size, left.kind, left.source) <
               std::tie(right.address, right.size, right.kind, right.source);
    };

    const auto to_fields = [](const Region& region) {
        std::vector<RecordField> fields{
            {.key = std::string{SizeKey}, .value = RenderAddress(region.size)},
            {.key = std::string{KindKey}, .value = std::string{Describe(region.kind)}},
        };
        if (region.instruction_set.has_value()) {
            fields.push_back({.key = std::string{InstructionSetKey},
                              .value = std::string{arm::Describe(region.instruction_set.value())}});
        }
        fields.push_back(
            {.key = std::string{SourceKey}, .value = std::string{Describe(region.source)}});
        return fields;
    };

    return RenderTable(Banner("Regions: what each stretch of the address space holds."),
                       std::move(records),
                       less,
                       to_fields);
}

core::Result<std::vector<CodePointer>> ParsePointers(std::string_view text) {
    return ParseTable<CodePointer>(text, [](const Record& record) -> core::Result<CodePointer> {
        CodePointer pointer;
        pointer.address = record.address;

        const core::Result<std::uint32_t> target = RequiredAddressField(record, TargetKey);
        if (!target) {
            return target.Failure();
        }
        pointer.target = target.Value();

        const core::Result<Provenance> source = ReadSource(record);
        if (!source) {
            return source.Failure();
        }
        pointer.source = source.Value();

        return pointer;
    });
}

std::string RenderPointers(std::vector<CodePointer> records) {
    const auto less = [](const CodePointer& left, const CodePointer& right) {
        return std::tie(left.address, left.target, left.source) <
               std::tie(right.address, right.target, right.source);
    };

    const auto to_fields = [](const CodePointer& pointer) {
        return std::vector<RecordField>{
            {.key = std::string{TargetKey}, .value = RenderAddress(pointer.target)},
            {.key = std::string{SourceKey}, .value = std::string{Describe(pointer.source)}},
        };
    };

    return RenderTable(
        Banner("Pointers: places in data that hold the address of code. The main\n"
               "executable has no relocation table, so these are found by analysis."),
        std::move(records),
        less,
        to_fields);
}

}  // namespace hexlith::domain::analysis
