# An application's memory under Horizon

What the kernel gives a 3DS application to run in, and how Hexlith reproduces it. The code is `kernel::AddressSpace` in `src/domain/kernel`, on top of `memory::ProcessMemory` in `src/domain/memory`.

Sources are 3dbrew's Memory layout and SVC pages and Azahar's kernel, `src/core/memory.h`, `src/core/hle/kernel/process.cpp` and `src/core/hle/kernel/memory.cpp`. Each address and rule below names the one it came from.

## The map

| Address | Size | What | Source |
| --- | --- | --- | --- |
| `0x00100000` | up to `0x03F00000` | The executable's code, read-only data and data, then its BSS | 3dbrew; the extended header gives the segments |
| `0x08000000` | `0x08000000` | Heap, committed by ControlMemory at addresses the game picks | 3dbrew; `HEAP_VADDR` |
| `0x10000000` minus the stack size | stack size | The main thread's stack, at the very top of the heap range | `Process::Run` |
| `0x10000000` | `0x04000000` | Shared memory blocks the kernel places | 3dbrew; `SHARED_MEMORY_VADDR` |
| `0x14000000` or `0x30000000` | the region's size | The linear heap | `GetLinearHeapAreaAddress` |
| `0x1FF80000` | `0x1000` | Configuration memory, read-only | 3dbrew; `CONFIG_MEMORY_VADDR` |
| `0x1FF81000` | `0x1000` | The shared page, read-only | 3dbrew; `SHARED_PAGE_VADDR` |
| `0x1FF82000` | `0x1000` a page | Thread-local storage, `0x200` bytes a thread | 3dbrew; `TLS_AREA_VADDR`, `TLS_ENTRY_SIZE` |

Nothing places the CRO modules at a fixed address. The game picks one when it calls LoadCRO, and the reference title puts its first module at `0x006A5000`, above the end of its static image. [`ldr-ro-trace.md`](ldr-ro-trace.md) has the measurement.

**Which linear heap.** The kernel compares the version the extended header's kernel capabilities declare with `0x22C`, which is 2.44: older titles get the linear heap at `0x14000000`, newer ones at `0x30000000`. The reference title declares 2.35, so its linear heap is at `0x14000000`. The `0x30000000` mapping arrived with system version 8.0.0, according to 3dbrew, which also says it replaces the old one for newer titles without saying how the kernel decides. Azahar's comparison is what decides it here.

## Physical memory

The console has one bank of FCRAM, and the kernel gives the application a region of it. Heap, linear heap, the executable and the main stack are all carved out of that region, which is why the game's heap can run out when its linear allocations grow.

The kernel takes from the region in two ways, and which one decides what the game sees:

- **Heap, code and stack** come from the top of the region down, in as many pieces as needed. The virtual addresses are the caller's, so the physical pieces need not be contiguous.
- **Linear memory** comes from the bottom, first fit, in one contiguous piece, and its virtual address is the linear heap base plus its offset in the region. So the first linear allocation of a game lands at the base, and each later one after the last.

Source for both: `MemoryRegionInfo::HeapAllocate` and `MemoryRegionInfo::LinearAllocate`. Keeping the policy exact keeps Hexlith's linear addresses the same as Azahar's, which a comparison between the two depends on.

Committed memory is zero filled, including memory given back earlier and taken again.

The region's size depends on the system's memory mode and is a parameter. 64 MiB is the application region on an Old 3DS in the default mode (`memory_region_sizes` in Azahar's `memory.cpp`).

## Memory states

Every page has a state, which QueryMemory reports and which the kernel checks before it lets a range change. The values are the kernel's: Free 0, Reserved 1, IO 2, Static 3, Code 4, Private 5, Shared 6, Continuous 7, Aliased 8, Alias 9, AliasCode 10, Locked 11 (3dbrew, SVC; Azahar's `vm_manager.h`).

| State | Given to |
| --- | --- |
| Code | the executable's text and read-only data |
| Private | its data and BSS, and committed heap |
| Locked | the main stack, the thread-local storage pages, and the source of a privileged alias |
| Continuous | committed linear memory |
| Aliased and Alias | the source and target of an ordinary MAP |
| AliasCode | the target of a privileged MAP |
| Shared | a mapped memory block |

## Aliases

ControlMemory's MAP operation maps the physical memory under one range at a second range as well. Two kinds exist, and the reference title needs the second before anything else runs.

**Ordinary**, from the game. The source must be heap and Private. Both stay usable, the source becomes Aliased and the target Alias.

**Privileged**, from a service acting for the game. This is what `ldr:ro` does with a module: the game reads the file into its heap, at `0x08044000` for the first module in the trace, and `LoadCRO` maps those pages at the address the module runs at, `0x006A5000`. The source becomes Locked and inaccessible, so the game cannot write the code it is running through the buffer, and the target becomes AliasCode with the access the service asks for. A privileged map of a range onto itself changes only the state and the access, which is how a module's own pages become executable.

UNMAP reverses either kind and gives the source the access the caller names. Source for all of it: `Process::Map` and `Process::Unmap`.

This is why guest memory is a page table rather than one flat allocation: two addresses have to reach the same byte. [ADR 0012](adr/0012-guest-memory-is-a-page-table.md) records the decision.

## Thread-local storage

Each thread gets `0x200` bytes, eight to a page, the pages mapped upwards from `0x1FF82000` as threads need them. A freed slot is reused before a new page is mapped. The thread finds its own through CP15's read-only thread ID register, and the IPC command buffer starts `0x80` bytes into it. Source: `Process::AllocateThreadLocalStorage`; 3dbrew, Thread Local Storage.

The console takes these pages from the BASE region, which the application otherwise cannot reach. Hexlith gives each page a backing of its own, which has the same effect: they do not come out of the application's region.

## What is not modelled

**Execute permission.** It is recorded and reported, and the interpreter does not check it, because it fetches through the same read as a load. Nothing in the game is known to depend on a fault for executing data.

**RESERVE, and the region flags of ControlMemory.** Azahar ignores both, and so does Hexlith for now.

**Memory-mapped I/O, VRAM and DSP memory.** The game reaches hardware through services, which is what the HLE answers, and nothing maps these ranges until a service needs them.
