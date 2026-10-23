# The reference frames

Phase 7's milestone is the title screen, both screens, drawn by the Vulkan backend from the GPU recording alone. What the backend is held to is the reference renderer of [`docs/pica-rasterizer.md`](pica-rasterizer.md), which is held to Azahar's software renderer in turn, and it is held to it on three frames of the recording to frame 1150 that [`docs/formats/gpu-recording.md`](formats/gpu-recording.md) describes.

| Frame | What the screens show | Why |
| --- | --- | --- |
| F1, 220 | The language prompt: a tiled background, a menu of rounded bars, text in several colours, a dialog box below | 2D: textures, alpha blending, text, the dialog's translucency |
| F2, 400 | The X logo forming out of light, with particles | Additive blending of many small sprites from the geometry shader |
| F3, 1100 | The title screen, PRESS START on the top screen and the video on the bottom | The milestone itself; the bottom screen is the video y2r:u converts |

Frame numbers are the recording's, which count vertical blanks as Hexlith's boot does. They are not Azahar's frame numbers, which run ahead or behind with the timing of its own boot: the same screen is about frame 300 in Azahar for F1 and 1100 for F3.

## Drawing them

```sh
hexlith-gpu-replay --frames 220,400,1100 <work>/<title>/gpu <work>/<title>/gpu-replay/hardware
hexlith-gpu-replay --sampling azahar --frames 220,400,1100 <work>/<title>/gpu <work>/<title>/gpu-replay/azahar
```

The tool replays the recording through `application::RenderGpuRecording`: every list through the reference renderer, every memory fill and display transfer through the transfer engines of [`docs/gpu-transfers.md`](gpu-transfers.md), and every framebuffer line into `domain::gpu::ScreenSetup`. A frame's picture is what its screens show once its framebuffer lines are in and before its own work starts. The pictures are the game's, so they go in the workspace and never in the repository. Replaying to frame 1100 takes three and a half minutes with the release build.

## How they are checked

There is no separate capture to compare with. Azahar run live does not reach the same frames with the same state: its boot takes a different number of frames, and what a frame draws depends on what the game did before it. Replaying the recording through Azahar's own GPU instead, as `scripts/compare_pica_rasterizer.py` does, gives Azahar's framebuffers for exactly the recording's frames, and with the reference sampling as Azahar does they agree with Hexlith's to the bit: VRAM, where both screens' framebuffers are, is the same after every one of the recording's 7,653 steps. What remains is turning a framebuffer into a picture, which `ScreenSetup` does and its tests cover, and which comes out upright for every frame looked at.

## Filtering

With the reference sampling as the registers say, bilinear and with mipmaps, rather than as Azahar does, the pictures change by this much, measured on 2026-10-18 over frames 220, 400, 700, 1000 and 1100:

| Screen | Pixels that differ | Mean difference a channel | 99th percentile | Largest |
| --- | --- | --- | --- | --- |
| Top, F1 220 | 24.1% | 1.07 | 20 | 81 |
| Bottom, F1 220 | 22.7% | 1.09 | 29 | 64 |
| Top, F2 400 | 35.4% | 0.44 | 5 | 135 |
| Top, 700 | 24.9% | 0.50 | 7 | 24 |
| Top, 1000 | 71.1% | 1.51 | 10 | 65 |
| Top, F3 1100 | 72.6% | 1.59 | 10 | 64 |

The bottom screen is black at frames 400 and 700 and shows the video at 1000 and 1100, which differs by at most 1. The largest differences are at the edges of text and sprites, where a filtered sample mixes in the transparent texels around a glyph. This is what the Vulkan backend, which filters, may differ from Azahar by; it is held to the reference in its filtering mode, where no such difference should arise.
