# 0019. The reference rasterizer follows Azahar's software renderer, and samples textures two ways

Status: Accepted
Date: 2026-10-18

## Context

The Vulkan backend needs something to be compared with that is simpler than itself: a rasterizer in software, written for clarity and run on the CPU, that turns the title's command lists into the pixels of its colour and depth buffers. Whether that reference is right has to be checked in turn, against something outside the project.

GBATEK documents the PICA200's registers, the formats of its buffers and textures, and the meaning of most settings. It does not document how the rasterizer decides which pixels a triangle covers, how finely it places vertices, how it interpolates, or the arithmetic of texture filtering and the level of detail. Nobody has published those, and this project has no console to measure them on.

Azahar's software renderer is the one description detailed enough to compare with pixel for pixel. It sets a triangle up in 12.4 fixed point, fills by a top-left rule its authors took from the PSP, interpolates in float32, and was tuned against hardware in places it says so: the texture combiners, fog, the depth and stencil tests. It does not filter textures at all. Every sample is the nearest texel of the largest mipmap level, whatever the texture unit's registers ask for, and the code says as much in a comment.

## Options

**Follow GBATEK and nothing else.** Where GBATEK is silent the reference would have to invent, and an invented rule cannot be told from a bug by comparing it with anything.

**Follow Azahar's software renderer exactly**, sampling included. The comparison can then demand identical bits, but the reference would draw minified textures with the shimmer the hardware's filtering exists to prevent, and the Vulkan backend, which filters as the registers say because that is what a GPU does, could not be held to it.

**Follow Azahar's software renderer, and sample two ways.** One way exactly as Azahar does, for the comparison; the other as the registers describe, bilinear and with mipmaps, for drawing and for the Vulkan backend to be held to.

## Decision

The third. `domain::gpu::Rasterizer` follows Azahar's software renderer for clipping, setup, the fill rule, interpolation, depth, texture fetches and wrapping, fragment lighting, the combiners, fog and the output merger, with two exceptions where GBATEK is the more specific source:

- The bits of GPUREG_LIGHTING_CONFIG1 that disable the reflection tables follow GBATEK, blue in bit 20 and red in bit 22, where Azahar has them the other way round.
- Blend equations 5 to 7 add, as GBATEK says; Azahar treats them as unknown and leaves the result undefined.

Fragment lighting is skipped only when GPUREG_LIGHTING_DISABLE says so, as Azahar does. GBATEK also has an enable, GPUREG_LIGHTING_ENABLE, and which of the two the hardware heeds is not known. In every draw of the reference title the two agree.

The procedural texture, cube maps, shadow textures, gas and shadow rendering, and the geometry shader's two modes that read inputs from uniforms are not implemented, and the reference reports a draw that asks for one.

`domain::gpu::Sampling` selects how a texel is fetched:

- `Sampling::AzaharSoftware` takes the nearest texel of the base level as Azahar computes it: the coordinate times the size, truncated.
- `Sampling::Hardware` filters as the texture unit's registers say: bilinear or nearest for magnification and minification, mipmaps chosen by the level of detail, linearly or not between levels. The level of detail is the larger rate of change of the coordinates in texels, as a power of two, plus the register's bias, within its least and greatest levels. How exactly the hardware rounds any of this is unknown.

`Hardware` is the default. Only `AzaharSoftware` can be compared with Azahar, and `scripts/compare_pica_rasterizer.py` uses it.

## Consequences

The reference can be compared with Azahar to the bit over every list of a recording, and is; `docs/pica-rasterizer.md` has the method and the result. A difference is then a bug on one side or the other, not a matter of tolerance.

In `Hardware` mode the reference and Azahar differ wherever a texture is filtered, and nothing outside the project says which filtering is right in the last bit. That difference is the reason the Vulkan comparison has a tolerance at all, and the ADR that sets it accounts for it.

Where Azahar is wrong about the hardware, the reference is wrong in the same way, and the comparison cannot see it. The places Azahar marks as guesses, the fill rule above all, are listed in `docs/pica-rasterizer.md` so that a later hardware oracle knows where to look first.
