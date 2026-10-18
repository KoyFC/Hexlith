# Azahar patches

`golden-trace.patch` makes Azahar write the golden trace of [`docs/golden-traces.md`](../../docs/golden-traces.md) into its log. It is applied to a checkout of Azahar, never built into Hexlith, and its context lines are Azahar's own code, under Azahar's licence, GPL-2.0-or-later.

```sh
git clone --recursive https://github.com/azahar-emu/azahar && cd azahar
git checkout 219d743a1 && git submodule update --init --recursive
git apply /path/to/hexlith/third_party/azahar/golden-trace.patch
```

It adds five log lines and names two sessions, nothing else: every request a service handles is logged with its header once the handler has run, every vertical blank of the top screen, every CRO load and unload with the module's address, and the file and directory sessions are named `fs:file` and `fs:directory` instead of the empty name they have upstream.

## The oracle patch

`oracle.patch` goes on top of the first, with `git apply` the same way. It adds one file of Hexlith's own, `src/core/hexlith_oracle.cpp`, licensed as the rest of Azahar, and calls into it: one at every vertical blank of the top screen, after the interrupts are signalled, one where HID reads the buttons, and, in the PICA command processor, one where it would draw, give a vertex in immediate mode, restart the primitive assembler or raise FINALIZE. The last only report while the oracle runs command lists for `pica-cases.bin`, and then the draw is not made; for `raster-cases.bin` the draws are made. It does nothing unless one of three environment variables is set:

| Variable | What it does |
| --- | --- |
| `HEXLITH_INPUT=<file>` | Presses buttons from an input script in Hexlith's format ([`docs/formats/input-script.md`](../../docs/formats/input-script.md)): `tap`, `press` and `release` of buttons, by vertical blank. Touch and the circle pad are not read |
| `HEXLITH_CAPTURE_DIR=<dir>` with `HEXLITH_CAPTURE_FRAMES=<n,n,...>` | At each of those frames writes the framebuffer each screen shows, as `frame-<n>-top.bin` and `frame-<n>-bottom.bin`, with its address, format and stride in a `.txt` beside it, then quits after the last. The bytes are what is in emulated memory, which only the software renderer keeps up to date |
| `HEXLITH_ORACLE_DIR=<dir>` | At the first vertical blank runs the case files in the directory through Azahar's own code, writes the results beside them and a `done.txt`, and quits |

A frame is counted as the golden trace counts it: the first vertical blank begins frame 1.

Azahar boots the title before it reaches a vertical blank, so it is given the cartridge image even for the oracle. The comparison scripts point `XDG_CONFIG_HOME` and `XDG_DATA_HOME` at a directory of their own, so that a person's Azahar profile is neither read nor written.

### Case files

Every number is little-endian.

`y2r-cases.bin` is a word with the count of cases, then each case: the input format, output format, rotation and block alignment as bytes, in y2r:u's values; the line width and the line count as halfwords; the eight coefficients as signed halfwords; the alpha as a byte and three bytes of padding; then the Y, U, V and YUYV buffers as they sit in memory, each a word with its length and then its bytes, empty if the format does not use it. Azahar answers in `y2r-results.bin`, each case a word with the length of its output and the output, which `tools/hexlith-oracle-run` writes as `y2r-results.hexlith.bin`. `scripts/compare_y2r.py` writes the cases and compares the two.

`pica-cases.bin` is a word with the count of cases. Each case is a word with its count of steps, and each step a word with a count of memory blocks, each block a physical address, a length and its bytes, then a command list as a length and its bytes. Every case starts from a state in which every internal register is zero but `IRQ_CMP(0)`, `IRQ_MASK` and `IRQ_AUTOSTOP`, which hold 12345678h, FFFFFFF0h and 1 as Nintendo's code leaves them, and every shader memory, uniform, attribute and table is zero. A step writes its blocks, puts its list at physical 185C0000h and runs it. `pica-results.bin` answers each step with a word counting its events, each a byte of kind (1 draw arrays, 2 draw elements, 3 immediate vertex, 4 primitive restart, 5 FINALIZE), a 64-bit FNV-1a hash of the 300h internal registers with the FIFO data ports zeroed, and a hash of the immediate vertex's inputs or zero; then sixteen section hashes in the order `docs/pica-commands.md` gives; then the 300h registers themselves. `tools/hexlith-oracle-run` writes `pica-results.hexlith.bin` the same way, and `scripts/compare_pica_commands.py` writes the cases and compares the two.

`shader-cases.bin` is a word with the count of cases, then each case: a word that is 0 for a vertex shader and 1 for a geometry shader, the entry point, the geometry shader's output mask, 4096 words of code, 4096 words of operand descriptors, 96 uniforms as four float32 bit patterns each, the bool uniforms as a word, the four integer uniforms as their registers hold them, then the registers as the run starts: 16 inputs, 16 temporaries and two banks of 16 outputs as four float32 bit patterns each, then the output bank, a0.x, a0.y, aL and the two condition flags, a word each. Azahar runs each through its interpreter and answers in `shader-results.bin` with the registers as the run left them, in the same layout, a word counting the primitives emitted, and each primitive as a word that is 1 if its winding was reversed and its three vertices of 16 attributes. `tools/hexlith-oracle-run` writes `shader-results.hexlith.bin` the same way, and `scripts/compare_pica_shaders.py` writes the cases and compares the two.

`texture-cases.bin` is a word with the count of cases, then each case: the format as GPUREG_TEXUNITi_TYPE holds it, the width and the height as words, then a word with the length of the texture and its bytes. Azahar answers in `texture-results.bin` with every texel of each texture, row by row in stored order, as four bytes of red, green, blue and alpha, and `tools/hexlith-oracle-run` writes `texture-results.hexlith.bin` the same way; `scripts/compare_pica_textures.py` compares them. `scripts/azahar_oracle.py` is what every comparison script runs Azahar with.

`raster-cases.bin` is a word counting the steps whose VRAM is wanted, those steps' indices as words, then a word with the count of steps and the steps themselves as a case of `pica-cases.bin` has them. There is one case, and before it all of VRAM and of the first 128 MiB of FCRAM is set to zero and the state reset as for `pica-cases.bin`. This time the PICA core draws: every triangle is rasterized into memory, which only the software renderer does, so the script configures Azahar for it and for its shader interpreter rather than its JIT. `raster-results.bin` answers each step with a 64-bit FNV-1a hash of the whole of VRAM after it, and each wanted step's VRAM is written whole as `raster-vram-<step>.bin`. `tools/hexlith-oracle-run` writes `raster-results.hexlith.bin` and `raster-vram-<step>.hexlith.bin` the same way, with the reference renderer sampling as Azahar's software renderer does, and `scripts/compare_pica_rasterizer.py` writes the cases and compares the two.
