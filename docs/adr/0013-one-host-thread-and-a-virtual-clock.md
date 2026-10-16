# 0013. The game's threads share one host thread and a virtual clock

Status: Accepted
Date: 2026-10-16

## Context

Pokémon X is multithreaded. Its crt0 creates an address arbiter before anything else, and the main executable creates threads, waits on events, mutexes and timers, and arbitrates on addresses in hundreds of places. Horizon schedules those threads by strict priority: the highest priority ready thread runs, a thread runs until it blocks or something of higher priority becomes ready, and there is no time slicing between equal priorities. An application's threads run on the first core, and can be given time on the second.

The HLE kernel has to reproduce that closely enough that the game never sees a schedule the console could not produce. A schedule that is merely plausible deadlocks rather than crashes: a thread that should have been preempted spins forever on a flag another thread never gets to set.

It also has to be debuggable. The way this project finds bugs is to run the same thing twice and compare, against Azahar or against an earlier run of itself, and a comparison is worthless if two runs of the same input differ.

## Options

**One host thread per game thread**, with host mutexes and condition variables standing in for the kernel's objects. Uses every host core, and gives up any control over the order things happen in. Two runs differ, a priority inversion the console would never show can happen, and a bug in the order of events cannot be reproduced.

**One host thread for all of them, scheduled by the HLE kernel**, switching only at system calls and timed events, with time measured in instructions executed. Deterministic. Gives up host parallelism, which an interpreter cannot use well anyway, and which lifted code can win back later per core rather than per thread.

**The second, with time measured in host time.** Simpler to relate to the outside world, and no longer deterministic: the same run on a loaded machine wakes timers at different points.

## Decision

The second. Every thread of the game runs on one host thread, one at a time, under `kernel::Scheduler`:

- The running thread is the highest priority ready thread, the first to become ready among equals. It runs until it waits, yields, exits, or a system call or an event makes a higher priority thread ready. A preempted thread goes back to the front of its priority, a woken one to the back. Source: the thread manager in Azahar, `src/core/hle/kernel/thread.cpp`.
- A wait ends, and a signalled object wakes its waiters, by the rules Azahar implements and the result codes it returns: highest priority waiter first, the first among equals, one-shot objects taken by the thread they wake. Mutexes lend their holder the priority of their best waiter.
- Time is a tick count, one tick an instruction executed at the ARM11's 268,111,856 Hz, which is also the unit `GetSystemTick` returns. It moves when instructions run and, when no thread is ready, jumps straight to the next timed event. Timeouts, sleeps and timers are events on that clock, as will be the GPU's vertical blank.

Threads the game puts on the second core are scheduled in the same queue as the others. That is a deviation, written down here so that it is found when it matters: on the console the second core runs alongside the first, and a high priority thread there cannot starve the first core. If the game turns out to depend on that, the scheduler grows one queue per core and runs them in turn.

## Consequences

A run is reproducible. The same cartridge, the same save and the same input produce the same instructions in the same order, so a divergence from Azahar or from an earlier run is found at the first instruction where it happens and can be found again.

Waiting costs nothing. A game idle on a vertical blank jumps the clock to the next blank instead of spinning, which is what makes an interpreter usable for reaching the first module at all.

One instruction is one tick, which the console's pipeline does not honour: a load that misses the cache costs more than an addition. Azahar counts the same way, so the two agree on what a tick is, and the game cannot tell except by timing itself against the outside world, which it has no access to.

Host parallelism is not used. When lifted code needs it, the place to add it is one host thread per guest core, each running this scheduler for its own core, synchronised only at the points where the console's cores synchronise.
