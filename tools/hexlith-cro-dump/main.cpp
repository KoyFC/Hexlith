#include "domain/ctr/cro.h"
#include "domain/ctr/cro_link_tables.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-cro-dump prints what the CRO reader makes of each module it is given,
// one record per line.
//
// It exists to be compared with Azahar. When Azahar loads a module it logs, for
// every module the new one links with, how many symbols it imports from it by
// index and by position, and scripts/read_ldr_ro_trace.py checks those lines
// against the import records printed here. The same tables are what the linker
// and the analysis read, so a disagreement is a bug in one reader or the other.
//
// A development tool, not part of the runtime. It reads the files it is named and
// never looks for the workspace.
namespace {

namespace core = hexlith::core;
namespace ctr = hexlith::domain::ctr;

constexpr int Success = 0;
constexpr int Failure = 1;

constexpr std::string_view Usage =
    "usage: hexlith-cro-dump <module>...\n"
    "\n"
    "Prints, for each CRO or CRS file named, one 'module' record with the size of\n"
    "every table, one 'import' record for each module it imports from, and one\n"
    "'hooks' record with the four segment tags of its header. Every number is\n"
    "decimal except the tags, which are hexadecimal.\n";

void Report(std::string_view message) {
    std::cerr << "hexlith-cro-dump: " << message << '\n';
}

std::vector<std::uint8_t> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::size_t AnonymousImports(const ctr::CroLinkTables& tables) {
    std::size_t total = 0;
    for (const ctr::CroImportModule& imported : tables.import_modules) {
        total += imported.anonymous.size();
    }
    return total;
}

void Print(const ctr::CroHeader& header, const ctr::CroLinkTables& tables) {
    std::cout << std::format(
        "module {} segments={} named-exports={} indexed-exports={} import-modules={} "
        "named-imports={} anonymous-imports={} external-relocations={} "
        "internal-relocations={} static-anonymous={} static-relocations={}\n",
        header.name,
        header.segments.size(),
        header.counts.named_exports,
        header.counts.indexed_exports,
        tables.import_modules.size(),
        tables.named_imports.size(),
        AnonymousImports(tables),
        tables.external_relocations.size(),
        tables.internal_relocations.size(),
        tables.static_anonymous_symbols.size(),
        tables.static_relocations.size());

    for (const ctr::CroImportModule& imported : tables.import_modules) {
        std::cout << std::format("import {} from={} indexed={} anonymous={}\n",
                                 header.name,
                                 imported.name,
                                 imported.indexed.size(),
                                 imported.anonymous.size());
    }

    std::cout << std::format("hooks {} unknown={:#010x} on-load={:#010x} on-exit={:#010x} "
                             "on-unresolved={:#010x}\n",
                             header.name,
                             tables.hooks.unknown.raw,
                             tables.hooks.on_load.raw,
                             tables.hooks.on_exit.raw,
                             tables.hooks.on_unresolved.raw);
}

bool Dump(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> bytes = ReadWholeFile(path);
    const core::ByteSpan module{bytes};

    const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(module);
    if (!header) {
        Report(std::format("{}: {}", path.string(), header.Failure().Message()));
        return false;
    }
    const core::Result<ctr::CroLinkTables> tables = ctr::ReadCroLinkTables(module, header.Value());
    if (!tables) {
        Report(std::format("{}: {}", path.string(), tables.Failure().Message()));
        return false;
    }

    Print(header.Value(), tables.Value());
    return true;
}

int Main(std::span<char* const> arguments) {
    if (arguments.size() < 2 || std::string_view{arguments[1]} == "--help") {
        std::cerr << Usage;
        return arguments.size() < 2 ? Failure : Success;
    }

    bool every_one_read = true;
    for (const char* argument : arguments.subspan(1)) {
        every_one_read = Dump(argument) && every_one_read;
    }
    return every_one_read ? Success : Failure;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        Report(thrown.what());
        return Failure;
    } catch (...) {
        Report("something was thrown that is not an exception");
        return Failure;
    }
}
