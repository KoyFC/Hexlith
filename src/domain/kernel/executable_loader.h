#ifndef HEXLITH_DOMAIN_KERNEL_EXECUTABLE_LOADER_H
#define HEXLITH_DOMAIN_KERNEL_EXECUTABLE_LOADER_H

#include "core/byte_reader.h"
#include "core/result.h"
#include "domain/ctr/exheader.h"
#include "domain/kernel/address_space.h"
#include "domain/kernel/system_pages.h"

#include <cstdint>

namespace hexlith::domain::kernel {

// Where the main thread starts, as the kernel sets it up. Source:
// SetupMainThread and ResetThreadContext in Azahar,
// src/core/hle/kernel/thread.cpp.
struct MainThreadStart {
    // The text segment's address: execution starts at its first instruction.
    std::uint32_t entry_point = 0;

    // The top of the heap range, which is where the main thread's stack ends.
    std::uint32_t stack_top = 0;

    // FPSCR for a new main thread: default NaN, flush to zero, round towards
    // zero, and the inexact flag set. Azahar's value, which the reference title
    // replaces with RunFast before it computes anything, as ADR 0010 records.
    std::uint32_t fpscr = 0;

    std::uint8_t priority = 0;
    std::uint8_t ideal_processor = 0;
};

constexpr std::uint32_t MainThreadFpscr = 0x03C00010;

// Builds the address space the kernel gives an executable: its three segments
// with their access, the BSS after the data, the main thread's stack at the
// top of the heap range, and the two system pages. Process::Run in Azahar,
// with the segment layout of its NCCH loader (src/core/loader/ncch.cpp).
//
// The code is the decompressed ExeFS .code, which holds the three segments one
// after another, each padded to whole pages, and must be exactly that size.
core::Result<MainThreadStart> LoadExecutable(AddressSpace& space,
                                             const ctr::ExHeader& header,
                                             core::ByteSpan code,
                                             const SystemPages& pages);

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_EXECUTABLE_LOADER_H
