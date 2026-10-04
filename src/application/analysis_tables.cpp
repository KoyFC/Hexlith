#include "application/analysis_tables.h"

#include <format>

namespace hexlith::application {
namespace {

namespace analysis = domain::analysis;

template<typename T, typename Parse>
core::Result<std::vector<T>>
ReadExisting(std::string_view text, Parse parse, std::string_view what) {
    if (text.empty()) {
        return std::vector<T>{};
    }
    const core::Result<std::vector<T>> parsed = parse(text);
    if (!parsed) {
        return parsed.Failure().WithContext(what);
    }
    return parsed.Value();
}

std::string PathIn(std::string_view directory, std::string_view name) {
    return directory.empty() ? std::string{name} : std::format("{}/{}", directory, name);
}

// Merges one table and writes it, answering how many records it holds.
template<typename T, typename Parse, typename Render>
core::Result<std::size_t> MergeAndWrite(const std::vector<T>& derived,
                                        std::string_view existing,
                                        Parse parse,
                                        Render render,
                                        analysis::MergeReport& merged,
                                        ports::BlobSink& sink,
                                        std::string_view path) {
    const core::Result<std::vector<T>> old = ReadExisting<T>(existing, parse, path);
    if (!old) {
        return old.Failure();
    }
    std::vector<T> records = analysis::MergeRecords(old.Value(), derived, merged);
    const std::size_t count = records.size();
    const core::Status wrote = sink.WriteText(path, render(std::move(records)));
    if (!wrote) {
        return wrote.Failure();
    }
    return count;
}

}  // namespace

core::Result<WrittenTables> MergeAndWriteTables(const DerivedTables& derived,
                                                const ExistingTables& existing,
                                                ports::BlobSink& sink,
                                                std::string_view directory) {
    WrittenTables written;

    const core::Result<std::size_t> symbols =
        MergeAndWrite(derived.symbols,
                      existing.symbols,
                      analysis::ParseSymbols,
                      analysis::RenderSymbols,
                      written.merged_symbols,
                      sink,
                      PathIn(directory, analysis::SymbolsFileName));
    if (!symbols) {
        return symbols.Failure();
    }
    const core::Result<std::size_t> functions =
        MergeAndWrite(derived.functions,
                      existing.functions,
                      analysis::ParseFunctions,
                      analysis::RenderFunctions,
                      written.merged_functions,
                      sink,
                      PathIn(directory, analysis::FunctionsFileName));
    if (!functions) {
        return functions.Failure();
    }
    const core::Result<std::size_t> regions =
        MergeAndWrite(derived.regions,
                      existing.regions,
                      analysis::ParseRegions,
                      analysis::RenderRegions,
                      written.merged_regions,
                      sink,
                      PathIn(directory, analysis::RegionsFileName));
    if (!regions) {
        return regions.Failure();
    }
    const core::Result<std::size_t> pointers =
        MergeAndWrite(derived.pointers,
                      existing.pointers,
                      analysis::ParsePointers,
                      analysis::RenderPointers,
                      written.merged_pointers,
                      sink,
                      PathIn(directory, analysis::PointersFileName));
    if (!pointers) {
        return pointers.Failure();
    }

    written.symbols = symbols.Value();
    written.functions = functions.Value();
    written.regions = regions.Value();
    written.pointers = pointers.Value();
    return written;
}

}  // namespace hexlith::application
