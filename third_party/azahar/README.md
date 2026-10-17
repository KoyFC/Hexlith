# Azahar golden trace patch

`golden-trace.patch` makes Azahar write the golden trace of [`docs/golden-traces.md`](../../docs/golden-traces.md) into its log. It is applied to a checkout of Azahar, never built into Hexlith, and its context lines are Azahar's own code, under Azahar's licence, GPL-2.0-or-later.

```sh
git clone --recursive https://github.com/azahar-emu/azahar && cd azahar
git checkout 219d743a1 && git submodule update --init --recursive
git apply /path/to/hexlith/third_party/azahar/golden-trace.patch
```

It adds five log lines and names two sessions, nothing else: every request a service handles is logged with its header once the handler has run, every vertical blank of the top screen, every CRO load and unload with the module's address, and the file and directory sessions are named `fs:file` and `fs:directory` instead of the empty name they have upstream.
