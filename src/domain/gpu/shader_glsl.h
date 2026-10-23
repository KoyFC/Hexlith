#ifndef HEXLITH_DOMAIN_GPU_SHADER_GLSL_H
#define HEXLITH_DOMAIN_GPU_SHADER_GLSL_H

#include "domain/gpu/pica_state.h"

#include <cstdint>
#include <string>

namespace hexlith::domain::gpu {

// A PICA vertex shader program as GLSL, to be compiled to SPIR-V and run on
// the host's GPU. ADR 0020 has why it is GLSL and why the translation keeps
// the interpreter's shape: a loop over the program counter with one case for
// each instruction the program can reach, so that every rule of
// RunShader's control flow, down to its faults, holds in the translation too.
//
// The text declares the shader's registers and a function
//
//     bool pica_run(uint limit)
//
// that runs the program from its entry point and answers whether it ended
// within `limit` instructions. It reads and writes these globals:
//
//     vec4 pica_v[16], pica_r[16], pica_o[16];  inputs, temporaries, outputs
//     ivec3 pica_a;                             a0.x, a0.y, aL
//     bvec2 pica_cmp;                           the condition flags
//
// and calls three functions the code around it must declare before it, so
// that the same translation serves a vertex shader and a compute shader that
// checks it:
//
//     vec4 pica_float(uint index);   float uniform c0 to c95
//     bool pica_bool(uint index);    bool uniform b0 to b15
//     uint pica_int(uint index);     integer uniform i0 to i3, as its register
struct GlslShader {
    std::string source;
    // The program can reach an instruction the translation does not
    // implement: EMIT and SETEMIT, which only a geometry shader may use, or
    // one RunShader does not implement either.
    bool unsupported = false;
};

// The code and operand descriptors are part of the translation; uniforms are
// not, so one translation serves every draw of the same program.
GlslShader TranslateShader(const ShaderUnitState& unit, std::uint32_t entry);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_SHADER_GLSL_H
