# The PICA shader interpreter, compared with Azahar

`domain::gpu::RunShader` runs vertex and geometry shader programs by the rules [ADR 0018](adr/0018-a-shader-interpreter-is-the-semantic-reference.md) sets out: float32 arithmetic with the behaviours Azahar's authors measured on hardware, GBATEK's control flow stacks, and nothing for the three instructions nobody has described with confidence. It is the reference the Vulkan backend's translated shaders are held to, so it is held to Azahar's interpreter first.

## Sources

GBATEK, 3DS GPU Shader Instruction Set - Opcode Summary and Blurp, for the encodings, the operand descriptors, relative addressing, the comparison operators, the conditions, and the control flow stacks and their order. Azahar's `src/video_core/shader/shader_interpreter.cpp` and `pica_types.h` for what GBATEK leaves open and what was measured: zero times infinity, the side MAX and MIN take with a NaN, the range of an address offset, the ones read past c95, the fault when four calls end on one instruction, a stack that drops its oldest scope when it is full, the last word of code memory read as END, and the loop counter a nested loop restores.

## The reference title's programs

Before frame 1150 the title runs four vertex programs and one geometry program, at entry points 0 and 260. Between them they reach 24 instructions: ADD, CALL, CALLC, CMP, DP3, DP4, EMIT, END, FLR, IFC, IFU, JMPC, JMPU, LOOP, MAD, MADI, MAX, MOV, MOVA, MUL, NOP, RCP, RSQ and SETEMIT. None of DST, DSTI, LITP, EX2, LG2, DPH, DPHI, SGE, SLT, MIN, BREAK, BREAKC or CALLU is reached.

## Method

`scripts/compare_pica_shaders.py` writes cases that both sides run: Azahar through `Pica::Shader::InterpreterEngine` at its first vertical blank, under `third_party/azahar/oracle.patch`, Hexlith through `RunShader` in `tools/hexlith-oracle-run`. A case is a whole unit: code and operand descriptor memory, uniforms, the entry point, and every register as the run starts. Every register is compared afterwards, the address registers and condition flags included, and for a geometry shader every primitive it emitted, vertex by vertex.

Three kinds of case:

- **One instruction.** A random state, then one random arithmetic, compare or multiply-add instruction with a random operand descriptor, then END. Floats are often a value that tests a rule: signed zeros, infinities, NaNs, a denormal, small integers. Address registers are often near the edges of a signed byte.
- **Random programs** of 2 to 40 instructions, two in five of them geometry shaders, mixing arithmetic with every kind of flow control: conditional and uniform IFs, CALLs and jumps, LOOPs over integer uniforms of up to five iterations, BREAKs, and EMIT and SETEMIT. Every target lies ahead of the instruction, so every program ends.
- **The title's own.** `hexlith-oracle-run --shader-cases-from` replays the recording and writes a case for each distinct state a draw ran a shader in: the program, descriptors and uniforms the title had loaded, with inputs drawn at random between -2 and 2.

Not compared: which NaN a NaN is, since which operand's NaN an operation passes on depends on the order the compiler puts the operands in, and the hardware's float24 has no payload to speak of; CMP with the operators 6 and 7, which GBATEK observed always hold and Azahar leaves as it found them; and an EMIT of a fourth vertex, which Azahar stops on.

## Results

On 2026-10-18:

| Cases | Agree |
| --- | --- |
| 200,000 single instructions and 50,000 random programs, seed 5 | all |
| 20,000 single instructions and 3,000 programs, seed 1 | all |
| 12,175 states of the title's own programs, from the recording to frame 1150 | all |

The comparison is not vacuous. Counting one loop iteration too few, as a check, made 319 of 3,000 random programs disagree.

## Where Hexlith departs from Azahar

| What | Azahar | Hexlith | Why |
| --- | --- | --- | --- |
| CMP operators 6 and 7 | the flag keeps its value | the flag is set | GBATEK observed the hardware setting it |
| DST, DSTI, LITP | nothing happens | nothing happens, and the run reports it reached one | the run's caller can say so, rather than draw something silently wrong |
| A vertex number of 3 in SETEMIT, then EMIT | an assertion | nothing is emitted, and the run reports it | the emitter holds three vertices |
| A program that never ends | runs for ever | stops after a limit, a million instructions by default, and reports it | |

Both keep one thing that looks like a fault of Azahar's own: when a LOOP starts, the counter it records to restore on leaving is the one it has just set, not the one the enclosing loop had. Only a loop inside another restores it, so the title's programs, whose one LOOP is not nested, never see the difference. Nothing settles what the hardware does.

## How much float24 would matter

`ShaderPrecision::Float24` rounds every result to float24, to nearest even with denormals flushed. `hexlith-oracle-run --precision` runs every vertex shader state of the title's programs both ways and measures how far the position, o0 in all of them, moves once divided by w, in pixels of a screen 400 pixels wide. Over the 6,367 random inputs that land on the screen, the median move is 0.00003 pixels and the largest 0.0035. The rasterizer resolves sixteenths of a pixel, so float24 arithmetic, as GBATEK describes the format, would change which pixels a triangle covers only for an edge that passes within a few thousandths of a sample point. The hardware's approximate reciprocals are not part of this, since nobody has published them.

## The GLSL translation, compared with the interpreter

The Vulkan backend runs vertex shaders translated to GLSL ([ADR 0020](adr/0020-pica-shaders-become-glsl-compiled-by-glslang.md)), and the translation is held to the interpreter the way the interpreter is held to Azahar. `scripts/compare_pica_shaders.py --against vulkan` writes the same kinds of case, runs them through `RunShader` and, with `hexlith-oracle-run --vulkan`, through the translation as a compute shader on a Vulkan device, one dispatch per program; geometry shader cases stay on the interpreter, as they do in the backend. For these runs the code memory around a random program holds END rather than random words, since the translation gives a case to every instruction a program could reach.

Differences are sorted three ways: to the bit; within what Vulkan lets a device be off by, 3 ULP, or a denormal against zero on a device that does not promise to keep denormals; and beyond.

On 2026-10-23:

| Cases | Device | To the bit | Within Vulkan's precision | Beyond |
| --- | --- | --- | --- | --- |
| 12,175 states of the title's programs | llvmpipe | all | | |
| 12,175 states of the title's programs | Intel Lunar Lake | 9,247 | 2,691 | 237 |
| 2,000 single instructions and 500 programs, seed 1 | Intel Lunar Lake | 2,466 | 34 | 0 |
| 2,000 single instructions and 500 programs, seed 1 | llvmpipe | 2,425 | 47 | 28 |

On llvmpipe, which computes in the host's floating point, the title's programs agree to the bit, which is what says the translation means what the interpreter means. The Intel GPU's differences within Vulkan's precision are all in RCP, RSQ and EX2, the operations Vulkan gives room. The 237 beyond it are those small differences carried through a program into a value that cancels or is normalised; 40 of them are a near-zero vector whose length one side finds to be a tiny positive number and the other a tiny negative one, so that its inverse square root is a NaN on one side only. None of them moves a vertex: over the 6,388 cases whose position lands on a screen 400 pixels wide, o0 is the same to the bit. llvmpipe's 28 beyond are all cases with a denormal operand, which it flushes to zero.

