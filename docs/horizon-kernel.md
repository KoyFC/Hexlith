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

## What is not modelled

**The second core.** Threads the game puts on it share the first core's queue. ADR 0013 records the deviation and what would change it.

**Resource limits beyond what the game reads.** The kernel caps threads, events, mutexes and the rest per process. Nothing in the reference title is known to come near the caps, and nothing here enforces them.
