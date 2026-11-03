#ifndef HEXLITH_APPLICATION_LIFT_EMIT_H
#define HEXLITH_APPLICATION_LIFT_EMIT_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "ports/blob_sink.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::application {

// Writes the lifted C++ of the main executable and what builds it, into the
// workspace.
//
// The output is a small CMake project: lifted.h with the declaration of every
// function, a shard of functions per source file so that compilation
// parallelises and a change rebuilds one, registry.cpp with the two functions
// the engine reads the library through, a manifest of what it was lifted from,
// and a CMakeLists.txt that builds it all into a shared library. None of it is
// ever checked in: it is derived from the game, and it is built from where it
// is.

// Bumped when the shape of the generated code or what it means changes, so that
// a library built by an older lifter is not mistaken for a current one.
constexpr int LifterVersion = 4;

// About how many guest instructions go in one source file. A file that is too
// big is slow to compile and a file that is too small has little to optimise
// across; this is a few seconds of compilation.
constexpr std::size_t DefaultInstructionsPerShard = 4000;

struct LiftEmitRequest {
    core::ByteSpan code;
    std::uint32_t base = 0;
    std::string_view functions;
    std::string_view regions;

    std::size_t instructions_per_shard = DefaultInstructionsPerShard;
};

struct LiftEmitReport {
    std::size_t functions = 0;
    std::size_t shards = 0;
    std::size_t blocks = 0;
    std::size_t instructions = 0;
    std::size_t source_bytes = 0;
    std::size_t source_lines = 0;

    // SHA-256 of the code that was lifted, which the library carries so that it
    // can be refused when the code it is loaded against is another.
    std::string code_hash;
};

core::Result<LiftEmitReport> EmitLiftedCode(const LiftEmitRequest& request, ports::BlobSink& sink);

std::string RenderLiftEmitSummary(const LiftEmitReport& report);

// One module to lift: the CRO file as the cartridge has it, and the analysis
// tables of that module, whose addresses are offsets into the file.
struct ModuleSource {
    // The name in the module's header, which is the loader's key for it.
    std::string name;
    core::ByteSpan file;
    std::string_view functions;
    std::string_view regions;
};

struct LiftModulesRequest {
    std::span<const ModuleSource> modules;
    std::size_t instructions_per_shard = DefaultInstructionsPerShard;
};

struct ModuleReport {
    std::string name;

    // SHA-256 of the module's code segment as the file has it: what the loader
    // compares with the code it is given, before it patches a word.
    std::string code_hash;

    std::size_t functions = 0;
    std::size_t blocks = 0;
    std::size_t instructions = 0;

    // Words of the code segment that a relocation writes. No instruction at one
    // is lifted, because the file does not hold what the loader will leave there.
    std::size_t relocated_words = 0;
};

struct LiftModulesReport {
    // Every module together. Its code hash is left empty: each module has its own.
    LiftEmitReport totals;
    std::vector<ModuleReport> modules;
};

// Writes the lifted C++ of the CRO modules, all in one library of their own.
//
// A module's code is lifted at offsets from its start and runs against the base
// the engine gives it (ADR 0029). Each module is a namespace, so that two
// modules with a function at the same offset do not meet, and has shards of its
// own. The output is a small CMake project like the main executable's, with a
// registry that hands each module's table of entries and code digest to the
// loader, and a manifest in text of the same.
core::Result<LiftModulesReport> EmitLiftedModules(const LiftModulesRequest& request,
                                                  ports::BlobSink& sink);

std::string RenderLiftModulesSummary(const LiftModulesReport& report);

// The names of the two functions the library exports, which the loader looks
// up by name.
constexpr std::string_view ManifestSymbol = "hexlith_lifted_manifest";
constexpr std::string_view RegisterSymbol = "hexlith_lifted_register";

// The same for the library of modules. Its register function takes a callback
// that is given each module's name, code digest and table, and a pointer to
// pass back, and the tables belong to the library.
constexpr std::string_view ModulesManifestSymbol = "hexlith_modules_manifest";
constexpr std::string_view ModulesRegisterSymbol = "hexlith_modules_register";

// Where the library of modules is built, given the path of the main
// executable's library: lift/build/libhexlith_lifted.so has
// lift/modules/build/libhexlith_modules.so beside it, which is where
// scripts/build-lifted.sh puts each.
std::filesystem::path ModulesLibraryBeside(const std::filesystem::path& lifted_library);

// The file that says in text what the library of modules holds.
constexpr std::string_view ModulesManifestFile = "manifest.txt";

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_LIFT_EMIT_H
