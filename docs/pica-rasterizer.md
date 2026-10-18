# The reference rasterizer, compared with Azahar

`domain::gpu::ReferenceRenderer` draws a command list in software: it loads each vertex from its attribute arrays, runs the vertex shader and, when the list asks for one, the geometry shader, assembles triangles, and hands them to `domain::gpu::Rasterizer`, which clips them, sets them up and runs every pixel they cover through texturing, fragment lighting, the texture combiners, fog and the output merger into the colour and depth buffers in GPU memory. It is the reference the Vulkan backend is held to. [ADR 0019](adr/0019-the-reference-rasterizer-follows-azahar-and-samples-two-ways.md) says why it follows Azahar's software renderer and why it samples textures two ways.

## Sources

GBATEK, 3DS GPU Internal Registers, for every register the reference reads: the Geometry Pipeline registers for the attribute arrays, the index buffer, the topology and the geometry shader's mode; Shader Output for the semantics of the shader's outputs; the Rasterizer registers for culling, the viewport, the custom clip plane, the scissor and the depth map; the Texturing registers for the texture units, the combiners, the combiner buffer and fog; the Fragment Lighting registers; and the Framebuffer registers for the alpha, stencil and depth tests, blending, the logic operation, the write masks, and the buffers' addresses, formats and size.

Azahar's `src/video_core/`, its software renderer in `renderer_software/` above all, for what GBATEK leaves open, which is most of the arithmetic. Paths below are from there, and a bare function name is in `renderer_software/sw_rasterizer.cpp`:

| Stage | Azahar | What it settles |
| --- | --- | --- |
| Vertex loading | `pica/vertex_loader.cpp`, `pica/pica_core.cpp` | alignment inside an array, the components an attribute of fewer than four leaves, the 64-vertex cache of an indexed draw, one set of shader registers for a whole draw |
| Primitive assembly | `pica/primitive_assembly.cpp`, `pica/geometry_pipeline.cpp` | the vertex a strip replaces, a reversed winding swapping the first two vertices, the geometry shader's inputs gathered from the vertex shader's outputs |
| Output vertex | `pica/output_vertex.cpp` | a semantic no output gives is one; the colour is made positive and kept to one |
| Clipping | `AddTriangle` | the six planes of the view volume, a tolerance of 10⁻⁸ on each, a seventh plane at w = 10⁻⁵, and the order Sutherland-Hodgman leaves the vertices in |
| Setup | `ProcessTriangle`, `renderer_software/sw_clipper.cpp` | 12.4 fixed point rounded to nearest, the fill rule, pixel centres at half a pixel, the scissor box in sixteenths |
| Interpolation | `ProcessTriangle` | attributes over w, interpolated with integer weights, divided by the interpolated one over w; depth from z over w, with w multiplied back in for a W-buffer |
| Texturing | `TextureColor`, `renderer_software/sw_texturing.cpp` | the coordinate times the size, truncated; rows counted from the bottom; wrap modes 4 to 7; a texture at address zero reading opaque black |
| Lighting | `renderer_software/sw_lighting.cpp` | every formula, the table inputs and their absolute and signed ranges, the configuration that provides each table |
| Combiners | `WriteTevConfig`, `renderer_software/sw_texturing.cpp` | each operation's integer arithmetic, the buffer read one stage late, the first stage reading its third source for a previous one |
| Fog | `WriteFog` | the table's index from the depth and the factor's interpolation |
| Output merger | `DoDepthStencilTest`, `PixelColor`, `renderer_software/sw_framebuffer.cpp` | the order of the tests, the stencil taken only with a D24S8 buffer, the action on passing taken with the depth test off, depth written with its test off, and the blend arithmetic |

Where Azahar marks something as a guess, the reference inherits it and cannot tell. In Azahar's own words: the fill rule is the PSP's, "not sure if the 3DS uses the same ones"; rounding to 12.4 "is necessary to prevent garbage pixels at triangle borders", without knowing whether it is right; the depth is "not fully accurate, about 3 bits in precision are missing"; fog is "not fully accurate"; DOT3 is within 3 of the hardware; the plane at w = 10⁻⁵ is "not sure if this is a valid approach". A hardware oracle should look there first.

## Method

`scripts/compare_pica_rasterizer.py` writes one case file that both sides run: Azahar at its first vertical blank under `third_party/azahar/oracle.patch`, with its software renderer and its shader interpreter, and Hexlith in `tools/hexlith-oracle-run`, with the reference sampling as Azahar does. Both start from GPU memory that is all zero and a PICA state reset as for the command list comparison. A step writes blocks of memory, then runs one list, drawing everything it asks for; after each step both hash the whole of VRAM, where the title keeps its colour and depth buffers. A step whose VRAM differs is run again to save VRAM whole, and the script says which bytes differ.

Two kinds of step:

- **The title's own.** Every list of the GPU recording of [docs/formats/gpu-recording.md](formats/gpu-recording.md), in order, each after the pages recorded before it. Neither side carries out the memory fills and display transfers between the lists, so the buffers are never cleared; that is the same on both.
- **Random.** The title leaves much of the fragment pipeline alone, so random steps cover the rest. Its 27,015 draws to frame 1150 always blend, sample one texture unit with a 2D texture, and never fog, use the stencil test or the scissor; the 8,735 that light their fragments do it with one light and every table off. Each is a list that loads a vertex shader copying seven inputs to position, quaternion, colour, the three texture coordinates and the view vector, then sets every register a draw reads to random values: culling, the scissor, the custom clip plane, the depth map, three texture units of random formats, sizes and wrap modes, all six combiner stages, the combiner buffer, fog and its table, all eight lights and the 24 lighting tables, and the whole output merger, with a colour buffer and a depth buffer of random formats and random contents. It then draws three to nine random vertices as a list, a strip or a fan.

The random steps keep out of two places. The viewport covers the 64-pixel buffers exactly, because Azahar does not check that a pixel is inside the buffer and reads and writes past it; Hexlith drops such a pixel. And the bits of GPUREG_LIGHTING_CONFIG1 that disable the red and blue reflection tables are kept equal, since Azahar has them the other way round from GBATEK (ADR 0019).

## Results

On 2026-10-18:

| Steps | Agree |
| --- | --- |
| The 4,915 lists of the recording to frame 1150: 27,015 draws and 2,068 vertices in immediate mode, 201,537 triangles, 460 million fragments | all |
| 2,000 random steps, seed 1 | all |
| 20,000 random steps, seeds 2 to 6 | all |

The comparison is not vacuous. An earlier version of the reference's lighting differed from Azahar's three ways, the light vector normalised by multiplying by the reciprocal of its length rather than dividing by it, the bump map's texel scaled by 2/255 rather than divided by 127.5, and the fourth bump mode keeping the default normal rather than none, and one random step in 200 disagreed with it. The random steps also found that a NaN depth indexed the fog table out of range, where Azahar's conversion reads its first entry.

## Where Hexlith departs from Azahar

| What | Azahar | Hexlith | Why |
| --- | --- | --- | --- |
| A pixel outside the colour or depth buffer | read and written, past the buffer's end | dropped | nothing outside the buffer belongs to it |
| GPUREG_LIGHTING_CONFIG1 bits 20 and 22 | 20 disables the red reflection table, 22 the blue | the other way round | GBATEK; ADR 0019 |
| Blend equations 5 to 7 | unknown, the result undefined | add | GBATEK |
| Gas and shadow rendering, cube maps, shadow textures, the procedural texture, the geometry shader's two modes that read uniforms | implemented, more or less | not, and the statistics report a draw that asks for one | the reference title uses none of them |

A float converted to an integer where it can be a NaN or out of range, in the texture coordinates, the lighting tables' indices, the fog table's index and the depth, is converted as Azahar's x86 build converts it, so that both sides agree even then. What the hardware does in those cases is not known.

## Not compared

`Sampling::Hardware`, which filters as the texture units' registers say, has nothing to be compared with: Azahar's software renderer does not filter, and its hardware renderers leave filtering to the host GPU. Nor is the reference compared with the hardware itself, so wherever Azahar is wrong about it, the reference is wrong the same way.
