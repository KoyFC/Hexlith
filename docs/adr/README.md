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

A record marked provisional has a named condition that will settle it. It is written down because work has to proceed in the meantime, not because the question is closed.
