# 0009. SHA-256 comes from OpenSSL's libcrypto

Status: Accepted
Date: 2026-10-03

## Context

The extractor identifies what it writes by content hash. The manifest records a SHA-256 for every file it produces, and the GameProfile records the hashes of the image a profile was written against, so that an unrecognised cartridge is refused rather than parsed with the wrong assumptions.

This is identification and not protection. There is no adversary, no secret and no timing concern. What matters is that the same bytes always produce the same digest, and that the digest agrees with what every other tool would compute for the same input, because a value nobody else can reproduce is not useful for cross-checking.

The volume is about four gigabytes per extraction: the two gigabyte image read once for identity, and the roughly 1.8 gigabytes of RomFS that the tool writes out.

At the time the question came up the development machine had the `openssl` program but not its development headers, so a dependency meant installing something.

## Options

**Implement it.** FIPS 180-4 is a fully specified algorithm that has not changed since 2001, and an implementation is about 150 lines. No dependency, and `src/core` keeps its property of needing nothing.

**OpenSSL's libcrypto.** Present on every platform the project targets, and it uses the processor's SHA extensions where they exist.

**A vendored single-header implementation**, such as picosha2. No system dependency, at the cost of carrying third-party code in the tree.

## Decision

OpenSSL's libcrypto, reached through the EVP interface, wrapped by `core::Sha256`.

The wrapper keeps every OpenSSL header inside its own translation unit, and the library is linked privately, so nothing else in Hexlith sees the dependency.

## Why

The measurement settled it. Hashing the reference image through the wrapper takes 0.6 seconds for 2 GiB and produces the same digest as `sha256sum`, because libcrypto dispatches to the processor's SHA instructions. A straightforward implementation in C++ runs an order of magnitude slower, which would have put a minute of hashing into a tool that is otherwise bound by how fast the disk can be read.

The second reason is how a hand-written implementation fails. The padding block at the end of a SHA-256 message carries the total length in bits as a 64-bit value. An implementation that keeps that counter in 32 bits overflows at 512 MiB and then produces a wrong digest for every larger input, and the wrong digest is 64 plausible hexadecimal characters that no amount of looking will distinguish from a right one. The cartridge image is four times past that boundary. The test vectors that catch it are the long ones, and the project would have been relying on code it wrote to verify code it wrote.

Against that, a package already shipped by every distribution and by every platform's package manager is the cheaper risk.

The vendored option was rejected because carrying third-party source to avoid a system package is the worse trade when the package is this ubiquitous, and because it has the same verification burden as writing it.

## Why the EVP interface

The older `SHA256_Init`, `SHA256_Update` and `SHA256_Final` have been deprecated since OpenSSL 3.0. CI builds with warnings as errors, so they do not compile at all here. `EVP_DigestInit_ex` and its companions compile clean under the full warning set on both gcc and clang, which was checked before the interface was chosen.

## Consequences

This is the project's first mandatory external dependency, and two documents said there was none. [`ARCHITECTURE.md`](../ARCHITECTURE.md) and `AGENTS.md` are amended rather than left to contradict the build.

CMake resolves it at configure time with `find_package(OpenSSL REQUIRED COMPONENTS Crypto)`, so a missing package is a configure error naming what is missing instead of a link failure later. The CI container installs `openssl-devel` alongside the compilers.

Windows will need a provider for it, through vcpkg or a system installation. Because the dependency is confined to one translation unit behind an interface of three methods, replacing it there is a contained change if that ever turns out to be painful.

The FIPS 180-4 test vectors stay in the test suite. They no longer test the algorithm, which OpenSSL is entitled to be trusted on, but they do test the wrapper: that SHA-256 was the digest asked for, that nothing is truncated, and that feeding the input in uneven chunks gives the same answer as feeding it all at once.

OpenSSL is Apache-2.0, which is compatible with GPL-3.0-or-later. It is recorded in [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) as a linked dependency rather than as adopted code, since no OpenSSL source is present in this repository.
