# The HLE kernel

The game's system calls land in `src/domain/kernel` instead of in the console's kernel. This note is what that code models and where each rule came from. The memory side, the address space and ControlMemory, is in [`horizon-memory.md`](horizon-memory.md).

Sources are 3dbrew's SVC page and Azahar's kernel, `src/core/hle/kernel/`. Every result code is the one Azahar returns, which is the closest record of the console's there is; its `errors.h` warns that the console itself is not consistent about which code it returns where.

## Objects and handles

A handle names a kernel object in the process's handle table. Its low 15 bits are a generation and its upper bits a slot, generations start at 1 and skip 0, slots are handed out lowest first and reused last freed first. So the first handle a process gets is `0x00000001` and the second `0x00008002`. Source: `HandleTable` in Azahar. The values reach the game, which stores and compares them, so the encoding is kept.

`0xFFFF8000` and `0xFFFF8001` stand for the calling thread and its process without being in the table.

| Object | Waits until | Taking it |
| --- | --- | --- |
| Thread | it has exited | changes nothing |
| Event | it is signalled | clears it if it is one-shot |
| Timer | it has fired | clears it if it is one-shot |
| Mutex | no other thread holds it | makes the taker its holder, or counts one more lock |
| Semaphore | its count is above zero | takes one from the count |

An event or a timer resets one of three ways (3dbrew, SVC, CreateEvent): one-shot stays signalled until a waiting thread takes it, sticky until it is cleared, and pulse wakes whoever is waiting at that moment and is unsignalled again straight after.

A mutex is recursive and lends its holder the priority of the best thread waiting for it, for as long as that thread waits. Only the holder may release it, and anyone else gets `0xD8E0041F`. A thread that exits releases every mutex it holds.

An address arbiter is not waited on through a handle. Threads wait on it at a guest address, and a signal wakes a number of the threads waiting at that address, highest priority first, the first among equals. Source: `AddressArbiter::ResumeHighestPriorityThread`, which notes the console picks the same way.

## Waking up

When an object becomes available, the waiting thread that can now take it and has the best priority wakes first, the first to have started waiting among equals; then the next, until no waiting thread can take it. A thread waiting for all of several objects wakes only when it can take every one of them, and takes them all. Source: `WaitObject::WakeupAllWaitingThreads` and `GetHighestPriorityReadyThread`.

A system call that puts a thread to sleep writes the answer for a timeout into its registers first: `0x09401BFE` in r0, and -1 in r1 for a WaitSynchronizationN. A signal overwrites r0 with success and, when the thread waited for any one of several objects, r1 with which one woke it. A timeout leaves them alone.

## Scheduling and time

[ADR 0013](adr/0013-one-host-thread-and-a-virtual-clock.md) records the model and why. In short: one host thread, strict priority, no time slicing, the running thread kept until it waits, yields, exits or is preempted by a higher priority thread becoming ready. A preempted thread goes back to the front of its priority and a woken one to the back.

Time is counted in ticks, one per instruction, at the ARM11's 268,111,856 Hz (`BASE_CLOCK_RATE_ARM11` in Azahar's `core_timing.h`). A nanosecond count converts to ticks rounded down. When nothing is ready to run, the clock jumps to the next timed event. When nothing is ready and nothing is scheduled, no thread can ever run again, and the kernel says so instead of spinning.

A yield, SleepThread with zero nanoseconds, does nothing when no other thread is ready. Otherwise the thread sleeps until the clock next moves, so every thread that was ready runs before it again. Source: `SVC::SleepThread` in Azahar.

A timer armed with no initial delay fires at once. A periodic one fires again every interval on the virtual clock, exactly, since nothing is ever late on it.

## System calls

The interpreter stops at an `svc` with its number, and `Kernel::Run` answers it before the thread runs on. A call is one indirect call through a table of 128 entries, one per number, each with the name 3dbrew gives it and the function that implements it, or none. A call with no function stops the run with its name and leaves the thread before the `svc`, so that once it is implemented the same run can carry on from there. Nothing is answered with a guess.

The arguments arrive in the registers the call's C prototype puts them in, with two changes the kernel makes to the ARM procedure call standard: outputs come back in r1 upwards after the result in r0, and arguments the standard would pass on the stack move into whatever registers are left. So SetTimer takes its initial delay in r2 and r3 and its interval in r1 and r4, and WaitSynchronizationN its timeout in r0 and r4. Source: `SVCWrapper` in Azahar, `svc_wrapper.h`, which derives each mapping from the prototype.

The walk of the main executable finds 32 different calls in code it reached, 750 call sites in all, every one of them ARM. The modules make none: they reach the kernel through the main executable. The extended header allows 51, and every one of the 32 is among them.

| Number | Call | Seen in the code | Implemented |
| --- | --- | --- | --- |
| `0x01` | ControlMemory | yes | yes |
| `0x02` | QueryMemory | | yes |
| `0x03` | ExitProcess | yes | yes |
| `0x08` | CreateThread | yes | yes |
| `0x09` | ExitThread | yes | yes |
| `0x0A` | SleepThread | yes | yes |
| `0x0B` | GetThreadPriority | yes | yes |
| `0x0C` | SetThreadPriority | yes | yes |
| `0x13` | CreateMutex | yes | yes |
| `0x14` | ReleaseMutex | yes | yes |
| `0x15` | CreateSemaphore | | yes |
| `0x16` | ReleaseSemaphore | | yes |
| `0x17` | CreateEvent | yes | yes |
| `0x18` | SignalEvent | yes | yes |
| `0x19` | ClearEvent | yes | yes |
| `0x1A` | CreateTimer | yes | yes |
| `0x1B` | SetTimer | yes | yes |
| `0x1C` | CancelTimer | yes | yes |
| `0x1D` | ClearTimer | | yes |
| `0x1E` | CreateMemoryBlock | yes | yes |
| `0x1F` | MapMemoryBlock | yes | yes |
| `0x20` | UnmapMemoryBlock | yes | yes |
| `0x21` | CreateAddressArbiter | yes | yes |
| `0x22` | ArbitrateAddress | yes | yes |
| `0x23` | CloseHandle | yes | yes |
| `0x24` | WaitSynchronization1 | yes | yes |
| `0x25` | WaitSynchronizationN | yes | yes |
| `0x27` | DuplicateHandle | | yes |
| `0x28` | GetSystemTick | yes | yes |
| `0x29` | GetHandleInfo | yes | yes |
| `0x2D` | ConnectToPort | yes | not yet |
| `0x32` | SendSyncRequest | yes | not yet |
| `0x35` | GetProcessId | yes | yes |
| `0x37` | GetThreadId | yes | yes |
| `0x38` | GetResourceLimit | yes | yes |
| `0x39` | GetResourceLimitLimitValues | | yes |
| `0x3A` | GetResourceLimitCurrentValues | yes | yes |
| `0x3C` | Break | yes | yes |
| `0x3D` | OutputDebugString | | yes |

The ones not seen in the code are cheap and allowed by the extended header, and the 20 percent of the text the walk does not reach may call them.

A few answers deserve a note.

**ControlMemory** checks alignment, then ignores the region bits of the operation, as Azahar does, and commits heap where the game asks and linear memory where the kernel picks. FREE takes heap or linear memory by the range the address is in.

**CreateThread** refuses a priority above the application's limit of `0x18`, which is the resource limit's Priority value, and maps processor -2 to the extended header's ideal processor and -1 to core 0. A new thread starts with r0 holding its argument and FPSCR `0x03C00000`.

**ArbitrateAddress** answers Timeout from the two kinds that take one whether or not the thread slept, which Azahar notes the console does. A signal that wakes nobody costs a thread of priority 49 or lower 1614 ticks, a figure Azahar takes from the console's kernel.

**The resource limit** reports the application's limits from Azahar's APPLICATION category: priority `0x18`, the region's size as committable memory, 32 threads, events and mutexes, 8 semaphores and timers, 16 memory blocks and 2 arbiters. The committed memory it reports as current is what the region has handed out, code and stack included.

**GetProcessId** answers 11 for the game itself. The console's number depends on how many system modules started first; 11 is what Azahar gave the reference title, and keeping it keeps the two traces alike.

**Memory blocks** a service hands out, and blocks the game makes without an address, come from the BASE region, which follows the application and system regions in FCRAM, and map by default at the linear address of their offset there. Blocks the game makes from its own heap lock that heap for as long as they exist.

Where Azahar would write an uninitialised value into an output register, Hexlith leaves the register as it was.

## What is not modelled

**The second core.** Threads the game puts on it share the first core's queue. ADR 0013 records the deviation and what would change it.

**Resource limits beyond what the game reads.** The kernel caps threads, events, mutexes and the rest per process. Nothing in the reference title is known to come near the caps, and nothing here enforces them.
