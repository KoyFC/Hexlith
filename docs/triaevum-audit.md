# TriAevum audit

[TriAevum](https://github.com/coccofresco/TriAevum) is a native recompilation of The Legend of Zelda: Ocarina of Time 3D for PC, licensed GPL-3.0-or-later. It is the closest existing work to Hexlith and the project's architecture reference.

Audited at commit `a9b4477`, 2026-10-03. 227 MB, 959 headers, 880 C++ sources, 683 Python files, 179 documents, and a root `CMakeLists.txt` of 3,538 lines declaring 143 targets.

This document records what Hexlith takes and what it leaves, and separates the parts that are generic to the 3DS from the parts that only make sense for Ocarina of Time 3D. No code from TriAevum has been copied into Hexlith yet; anything adopted later is recorded in `THIRD_PARTY_NOTICES.md` with its origin and licence.

## Reusable, generic

| Component | What it is | Licence | Note |
| --- | --- | --- | --- |
| `tools/triaevum_release/ctr_rom.py` | NCSD, NCCH and ExeFS parsing with careful bounds checking, plus the backwards-LZSS decoder for compressed `.code` | GPL-2.0-or-later, derived from Citra and Azahar | The basis for Phase 1. Its decoder was run against the Pokémon X image and produced 5,156,864 bytes, matching the sum of the extended header's page-aligned segments exactly |
| `tools/oot3d/third_party/azahar_pica` | Azahar's `video_core` and the `nihstro` shader library | GPL-2.0-or-later and MIT | The PICA200 donor for the graphics phase |
| `tools/oot3d/third_party/azahar_audio` | Azahar's `audio_core/hle`: source, mixers, filter, codec, interpolation | GPL-2.0-or-later | The DSP donor for the audio phase |
| Its GPL compliance layout | `LICENSE_SCOPE.md`, `THIRD_PARTY_NOTICES.md`, `SOURCE_OFFER.md`, donors vendored under `third_party/` with their own licence files | | Adopted as a pattern |

## Adaptable: good idea, implementation tied to Ocarina

| Component | The idea worth taking | What has to change |
| --- | --- | --- |
| The Forge pipeline | Local extraction from the user's own image into a private working directory, content-addressed identity, a manifest of sizes and hashes, and a cache so unchanged inputs are not rehashed | Forge selects the first usable partition. Hexlith validates against a `GameProfile` and refuses an image it does not recognise, because selecting the wrong partition silently is worse than failing |
| `whole_aot_cpp.py` and `whole_aot_optimization_ir.py` | Per-function ARM to C++ emission, output sharded for incremental compilation, and liveness analysis on the condition flags | The intermediate representation is Python dictionaries serialised to JSON. Hexlith's is typed C++, for the reasons in ADR 0001 |
| `docs/OOT3D_TRUE_AOT_CONTRACT.md` | A resolution order per function, an explicit host ABI with a context struct and a classified exit, and a guest program counter to source line map for diagnostics | A solid contract. The document is written in Italian, and it assumes a single fixed-address `code.bin` with no dynamic modules |
| `CtrHostServices` | A single boundary between guest system calls and host implementations | A flat interface of about twenty virtual methods. Hexlith uses a service interface with a registry keyed by port name, so a service can be added without touching the others |
| `triaevum/service_abi.h` | A versioned C ABI with four-character codes per subsystem, so a generated title module can be loaded as a plugin | Premature for Hexlith. The versioned ABI idea is worth remembering for when packaging matters |
| `runtime/.../fast/renderer3ds` and `nri_pica_*` | The bridge from PICA state to a modern graphics API | Interleaved with the Nintendo 64 renderer. Only the `renderer3ds` part is relevant, and it would need extracting from the surrounding code |

## Discard

| Component | Why |
| --- | --- |
| `runtime/three_ds_recomp/src/fast` and `include/fast` | Fast3D and libultraship. Despite the name, `src/fast/interpreter.cpp` is an interpreter for Nintendo 64 GBI display lists, not for ARM code. Nothing here applies to the PICA200 |
| `runtime/three_ds_recomp/src/ship`, `src/legacy_compat`, `include/.../libultra` | Ship of Harkinian and libultra. Nintendo 64 infrastructure |
| `include/fast/oot3d/pica_toon_fragment_binaries.h` | 99 MB of generated shader binaries committed as a C++ header, with another 10 MB beside it. The reason Hexlith's data guard has a size tripwire |
| `tools/oot3d/source_imports` | Decompiled game source. Excluded by Hexlith's first rule regardless of its technical merit |
| `oot3d_asset_tool`, `typed_gameplay`, `ui_topscreen`, `ui_n64`, the grass and TopScreen work, `decomp_support` | Specific to Ocarina of Time 3D |
| The root `CMakeLists.txt` | 3,538 lines and 143 targets in one file. Hexlith uses one `CMakeLists.txt` per directory |
| Its `.gitignore` | Inherited from an unrelated Nintendo 64 decompilation project. It excludes `*.z64`, `*.v64` and `tools/mipspro7.2_compiler/`, ignores `*.png` globally, and matches nothing a 3DS extraction produces |

## The gap that matters

**TriAevum has no CRO support.** Searching its sources for `CRO0`, `ldr:ro` or `LDR_RO` turns up only incidental mentions in Python analysis scripts.

That is expected. Ocarina of Time 3D is a single fixed-address image. Pokémon X loads 49.4 percent of its executable code from 133 dynamically relocated modules, as measured in [`formats/cro0.md`](formats/cro0.md).

So the single hardest part of Hexlith has no precedent in the reference project, and the `ldr:ro` reimplementation, the CRO parser and position-independent lifting are all new work. See [ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md).

## Process observations

These are not criticisms of a working project that does something impressive. They are the specific things Hexlith does differently, and why.

**179 documents and no decision records.** The documentation is almost entirely status and handover notes, with titles like `AOT_RENDICONTO_2026-09-17.md` and `NRI_RENDERER_RECOVERY_STATUS.md`, in a mixture of English and Italian. Nothing states a decision with its alternatives. The two observations are probably related: when decisions are not recorded, they get revisited, and revisiting generates status documents. Hexlith keeps `docs/adr/` for decisions and expects far fewer documents overall.

**Commit conventions are mixed.** Some commits use Conventional Commits prefixes and most are free-form sentences. Hexlith picked one convention and enforces the mechanical parts of it in a hook.

**The guardrail priorities are inverted.** Its `pre-commit` configuration runs only `clang-format`, pinned to version 14 while the codebase is modern C++, and there is no check for game-derived content at commit time. Protection against committing title content lives in a release-policy workflow, which runs after the commit exists. Hexlith puts the content check in the pre-commit hook and the formatter alongside it.

**Generated artefacts are committed.** The 99 MB shader header is the clearest case. Hexlith's size tripwire and entropy check exist to make this impossible by accident.

## Licensing

TriAevum's original contributions are GPL-3.0-or-later. Its Azahar-derived code is GPL-2.0-or-later, which permits combination under GPL-3.0-or-later.

Hexlith is GPL-3.0-or-later, so adopting either is compatible. Each adoption is recorded in `THIRD_PARTY_NOTICES.md` with the upstream project, the file, the licence and the SPDX identifier, and donor code keeps its original copyright headers.

Nothing derived from the game is adopted from any project, under any licence.
