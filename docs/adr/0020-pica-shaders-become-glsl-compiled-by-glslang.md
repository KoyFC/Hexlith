# 0020. PICA vertex shaders become GLSL that keeps the interpreter's shape, compiled by glslang

Status: Accepted
Date: 2026-10-23

## Context

The Vulkan backend has to run the title's vertex shaders on the host's GPU, and Vulkan takes SPIR-V. Something has to turn a PICA program, the code and operand descriptors a list uploads, into SPIR-V at run time, since the programs are the game's and none of them, translated or not, may enter the repository. Whatever does it has to be checked against the interpreter of [ADR 0018](0018-a-shader-interpreter-is-the-semantic-reference.md), which is checked against Azahar.

PICA control flow does not map onto structured control flow. CALL, IF and LOOP push scopes that end at an address rather than a matching instruction, a JMP may leave any of them, the stacks drop their oldest entry when full, and four calls ending at once leave the counter where it was. The title's programs use CALL, CALLC, IFC, IFU, JMPC, JMPU and LOOP.

Geometry shaders emit a varying number of vertices with SETEMIT and EMIT, which Vulkan's geometry stage could take but which neither of the reference GPUs here, an Intel integrated one and Mesa's llvmpipe, would make simpler.

## Options

**SPIR-V written directly**, with a builder of Hexlith's own. No new dependency, but SPIR-V is verbose to write and hard to read, and every structured-control-flow rule of SPIR-V would be Hexlith's to get right.

**GLSL, compiled to SPIR-V by glslang**, the Khronos reference compiler, linked as a library. One new build dependency, packaged everywhere Vulkan is. The translation is text a person can read, the compiler validates what it produces, and glslang's errors point at a line of it.

**One shader that interprets PICA code on the GPU**, reading the code from a buffer. Nothing to translate, but every vertex pays for decoding every instruction, and it is a second interpreter to keep in step with the first.

For the shape of the translation: **structured**, recovering ifs and loops from the scopes, which is fast but has to give up wherever PICA's flow is not structured; or **a loop over the program counter**, one case for each instruction the program can reach, with the scopes on small stacks as the interpreter keeps them.

## Decision

GLSL, compiled by glslang, in the shape of the interpreter. `domain::gpu::TranslateShader` turns a program and an entry point into a function `pica_run` that loops over the counter: each case is one instruction with its operands, selectors and negations already decoded, and after each the scopes close by the interpreter's rules. Only the instructions an analysis finds reachable from the entry point get a case: every successor, and for flow control its target, the address after it and the end of the scope it opens. Uniforms are read through functions the surrounding code supplies, so one translation serves every draw of a program, and the same translation runs inside a compute shader that checks it against the interpreter.

The arithmetic follows ADR 0018: products are `pica_mul`, zero times infinity being zero; MAX and MIN pick their side with a NaN; MOVA's conversion makes a NaN the most negative integer; and every result is `precise`, so that a compiler may not fuse a product into a following sum.

The SPIR-V asks the device to keep signed zeros, infinities and NaNs, and denormals, through the SignedZeroInfNanPreserve and DenormPreserve execution modes of Vulkan 1.2's float controls, as far as the device promises them. Without them a driver may assume no NaN ever happens and drop the very test `pica_mul` makes.

Geometry shaders stay on the interpreter, on the CPU: the title runs one geometry program, in 1,737 of its 27,015 draws, and the backend draws what it emits.

The new build dependencies are Vulkan (headers and loader) and glslang. The maintainer approved them for Phase 7.

## Consequences

The translation can be compared with the interpreter case by case, and is, by `scripts/compare_pica_shaders.py --against vulkan`; `docs/pica-shaders.md` has the results. On llvmpipe every state of the title's programs agrees to the bit. On a GPU, division, inverse square root, exp2 and log2 may be off by the 2 to 3 ULP Vulkan allows, which no execution mode can forbid; for the title's programs that never moves a vertex on the screen.

A loop over the counter is slower than structured code would be, and a program with a great many reachable instructions makes a large shader that a driver compiles slowly. The title's vertex shaders, as the code memory stood at each draw, make ten distinct translations of at most 117 KB of GLSL, which compile in four seconds together. If drawing shows the loop to matter, structured translation can be added for the programs whose flow allows it, with this one as what it is checked against.

When a device cannot keep denormals, as llvmpipe cannot, a program given denormal inputs differs from the interpreter wherever they would have mattered. The title's vertex data has none.
