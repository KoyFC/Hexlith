# 0012. Guest memory is a page table

Status: Accepted
Date: 2026-10-16

## Context

ARCHITECTURE.md planned guest memory as one flat host allocation with the 3DS memory map laid out inside it, so that a guest address becomes a host address by adding a constant.

That cannot express what the reference title needs before its first module runs. `ldr:ro` maps the heap buffer the game read a module into at the address the module runs at: `0x08044000` and `0x006A5000` for the first module in the trace, two addresses for one memory. ControlMemory's MAP operation and MapMemoryBlock do the same for the game itself. In a flat allocation every address has its own bytes, so an alias would need copying both ways on every write, or a check on every access, and both are wrong in the places nobody looks.

The address space is also sparse: a heap at `0x08000000`, a linear heap at `0x14000000`, thread storage at `0x1FF82000`. A flat allocation would have to cover half a gigabyte of address space to hold them.

## Options

**One flat allocation**, with aliases kept in sync by copying. Cheapest access; correct only as long as every write path remembers to copy.

**A page table**: an array with one host pointer per 4 KiB guest page, pointing into backings that stand for physical memory. An access looks up its page and adds the offset. Two pages pointing at the same backing are an alias with nothing to keep in sync.

**A sorted list of mapped ranges**, searched on every access. Flexible, and a search per access.

## Decision

A page table. `memory::ProcessMemory` holds one host pointer and one access byte per page, a million entries for the 4 GiB space, nine megabytes in all. Backings are reference counted, so a backing lives as long as any page points into it. Physical memory is a backing for the application's FCRAM region that `kernel::AddressSpace` carves heap, linear heap, code and stack out of, the way the kernel does.

An access that stays inside a page, which every aligned access does, is an array index, a permission test and a host load or store. One that straddles two pages goes byte by byte and fails as a whole.

## Consequences

Aliases cost nothing and need no care: a write through one name is visible through every other.

An access is an indexed load more than the flat design. That is the price of correctness here, and it is the same structure Azahar uses for the same reason. If it shows up in a profile of the interpreter or of lifted code, the place to recover it is a translation of whole runs of pages for code that knows its addresses are plain heap, not a return to the flat allocation.

The page table is per process and owns no bytes, so a second process, which the game never creates, would cost nine megabytes of table and nothing else.

The interpreter is instantiated for `ProcessMemory` as well as for the paged memory the tests and tools use, so neither pays for the other.
