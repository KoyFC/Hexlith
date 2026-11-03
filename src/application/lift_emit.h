#ifndef HEXLITH_APPLICATION_LIFT_EMIT_H
#define HEXLITH_APPLICATION_LIFT_EMIT_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "ports/blob_sink.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

// The names of the two functions the library exports, which the loader looks
// up by name.
constexpr std::string_view ManifestSymbol = "hexlith_lifted_manifest";
constexpr std::string_view RegisterSymbol = "hexlith_lifted_register";

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_LIFT_EMIT_H
