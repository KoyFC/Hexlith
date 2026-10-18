# 0017. The PICA state is its register file, and a list is walked with no virtual call

Status: Accepted
Date: 2026-10-18

## Context

Phase 7 draws what the game handed the GPU. What it handed over is command lists, and a command list is a run of writes to the PICA200's internal registers. Most writes only store a value. A few do something as well: upload a word of shader code, push a component of a uniform into a queue, step an index into a lookup table, start a draw, jump to another list, raise the interrupt that ends one.

Three things will read the state those writes build: a software rasterizer that is the reference, a Vulkan backend, and comparison tools that set both against Azahar. The state has to be one thing all three read the same way, and it has to be cheap to keep up to date, because the reference title writes 951,147 uniform words before its title screen and every one of them goes through it.

## Options

**A struct per register group with bit fields**, as Azahar has it: readable at the point of use, and a second copy of the layout that has to be kept in step with the raw words a list writes, whose padding and aliasing are easy to get wrong and impossible to compare with a dump of the raw words.

**The raw register file as the only truth, with functions that decode a field when it is needed.** What a list writes is what is stored, so a comparison with Azahar is a comparison of two arrays of words. A field is decoded where it is used, by a function that names it and cites where its bit positions come from.

**A virtual interface for what consumes the writes**, one implementation per backend: the simplest to wire, and a virtual call for every register write and every draw, which `AGENTS.md` rules out on this path.

## Decision

The register file is the state: 1,024 words, one per register ID the command header can name (GBATEK, Command Lists: IDs 0000h to 03FFh). A write stores its value under its byte mask, whatever the register is, so that the array always holds what the game last wrote.

What is not a register sits beside it in `domain::gpu::PicaState`, each piece in the form the hardware fills it: the code and operand descriptor memories of the vertex and geometry shader units, their float uniforms, the queues that assemble a uniform or a fixed attribute from three or four words, the fixed attributes, the lighting, fog and procedural texture lookup tables. Float uniforms and attributes are kept as host floats converted from float24 or float32 without loss, so that the arithmetic the shader interpreter does with them is decided there and not here.

`PicaState::Write` is the one place a write's side effect happens, and it answers with what else the write asks for: nothing, a draw, a jump, the end of the list, a vertex given in immediate mode, a change of primitive topology. `CommandProcessor<Executor>` walks a list, follows its jumps through GPU memory, and hands each request to its executor, a template parameter. The reference rasterizer, the Vulkan backend and the comparison tools are executors. No call on the way from a command word to a draw is virtual.

The decoded fields are functions over the register file in `pica_registers.h`, named after GBATEK's register names, each with the source of its bit positions.

## Consequences

Comparing Hexlith's state with Azahar's after a list is a comparison of words, which is what `scripts/compare_pica_commands.py` does.

A field read in a hot loop is a shift and a mask on a word already in cache. A backend that wants a struct can build one per draw from the functions.

The register file holds IDs Azahar does not keep (it stops at 0300h) and writes Azahar handles differently, such as the unmasked value it hands to a shader upload. Where the two differ the difference is recorded in `docs/pica-commands.md` with what settles it.

The executor is chosen at compile time, so a tool that wants two backends at once instantiates the processor twice. That is the price of the hot path having no indirection, and it is paid once per tool.
