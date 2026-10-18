# Azahar patches

`golden-trace.patch` makes Azahar write the golden trace of [`docs/golden-traces.md`](../../docs/golden-traces.md) into its log. It is applied to a checkout of Azahar, never built into Hexlith, and its context lines are Azahar's own code, under Azahar's licence, GPL-2.0-or-later.

```sh
git clone --recursive https://github.com/azahar-emu/azahar && cd azahar
git checkout 219d743a1 && git submodule update --init --recursive
git apply /path/to/hexlith/third_party/azahar/golden-trace.patch
```

It adds five log lines and names two sessions, nothing else: every request a service handles is logged with its header once the handler has run, every vertical blank of the top screen, every CRO load and unload with the module's address, and the file and directory sessions are named `fs:file` and `fs:directory` instead of the empty name they have upstream.

## The oracle patch

`oracle.patch` goes on top of the first, with `git apply` the same way. It adds one file of Hexlith's own, `src/core/hexlith_oracle.cpp`, licensed as the rest of Azahar, and two calls into it: one at every vertical blank of the top screen, after the interrupts are signalled, and one where HID reads the buttons. It does nothing unless one of three environment variables is set:

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

