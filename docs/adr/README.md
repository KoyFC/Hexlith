# Architecture decision records

One file per decision that would be expensive to reverse or confusing to rediscover. Numbered, dated, and never deleted. A decision that turns out wrong gets superseded by a new record rather than edited, so that the reasoning behind the original choice stays readable.

The format is deliberately short: context, the options that were actually considered, the decision, and what it costs.

| Number | Decision | Status |
| --- | --- | --- |
| [0001](0001-language-and-build-system.md) | C++20 and CMake, for tools as well as the runtime | Accepted |
| [0002](0002-lifter-emits-cpp-not-llvm-ir.md) | The lifter emits C++ rather than LLVM IR | Accepted |
| [0003](0003-interpreter-as-the-semantic-reference.md) | An ARM interpreter is the semantic reference | Accepted |
| [0004](0004-cro-modules-and-the-ldr-ro-hle.md) | CRO modules are lifted position-independently | Accepted |
| [0005](0005-game-profile-design.md) | GameProfile splits compiled identity from text tables | Accepted, amended by 0011 |
| [0006](0006-analysis-layer-format.md) | Analysis artefacts are line-oriented text with provenance | Accepted, amended by 0011 |
| [0007](0007-no-game-data-in-the-repository.md) | No game-derived data in the repository, enforced mechanically | Accepted |
| [0008](0008-workspace-configuration.md) | Workspace paths come from configuration, never from code | Accepted |
| [0009](0009-sha256-from-openssl.md) | SHA-256 comes from OpenSSL's libcrypto | Accepted |
| [0010](0010-floating-point-on-the-host.md) | VFP arithmetic runs on the host's floating point | Accepted |
| [0011](0011-only-underivable-records-in-the-repository.md) | Only records nothing can derive are committed | Accepted |
| [0012](0012-guest-memory-is-a-page-table.md) | Guest memory is a page table | Accepted |
| [0013](0013-one-host-thread-and-a-virtual-clock.md) | The game's threads share one host thread and a virtual clock | Accepted |
| [0014](0014-a-stand-in-for-the-shared-font.md) | The shared font is a stand-in built from the format | Accepted |
| [0015](0015-save-data-lives-in-memory-and-is-written-on-commit.md) | Save data lives in memory and is written on commit | Accepted |
| [0016](0016-the-gpu-recording-keeps-lists-and-changed-pages.md) | The GPU recording keeps the lists and the pages that changed | Accepted |
| [0017](0017-pica-state-is-the-register-file.md) | The PICA state is its register file, and a list is walked with no virtual call | Accepted |
| [0018](0018-a-shader-interpreter-is-the-semantic-reference.md) | A PICA shader interpreter is the semantic reference, and it calculates in float32 | Accepted |

A record marked provisional has a named condition that will settle it. It is written down because work has to proceed in the meantime, not because the question is closed.
