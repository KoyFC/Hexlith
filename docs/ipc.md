# Services and IPC

The game reaches everything outside its own process, the file system, the GPU, the applet manager and the rest, by sending requests to services. On the console each service is a process of its own. In Hexlith it is host code that answers in the game's process, called from the kernel when the game makes the request. This note is how a request travels and what the services answer.

Sources: 3dbrew's IPC page for the command buffer, and its page for each service for the commands. The answers are the ones Azahar's service implementations give, in `src/core/hle/service/`.

## Opening a service

The game connects to one port by name, `srv:`, with ConnectToPort, and gets a session handle. Through that session it asks the service manager for every other service by name, with GetServiceHandle, and gets a session handle for each.

A process may only open the services its extended header lists. The reference title lists 28. srv: refuses any other name with `0xD8E06406`, as the console's does (3dbrew, SRV), so the list is also the set of services Hexlith ever has to answer. Each of the 28 is registered from the start, and until it is implemented it answers nothing: the first command the game sends it stops the run with the service's name and the command's header. A command is implemented when a run reaches it, not before.

## The command buffer

A thread writes a request into its own thread-local storage, 0x80 bytes in, and calls SendSyncRequest with the session handle. The reply comes back in the same place.

The first word is the header: the command ID in bits 16 to 31, the number of normal words in bits 6 to 11, the number of translate words in bits 0 to 5. Normal words follow, then translate words, which the kernel rewrites on the way:

| Descriptor | Layout | What the kernel does |
| --- | --- | --- |
| Handles | type 0; bit 4 moves; count less one in bits 26 to 31 | copies each object to the receiver, and closes a moved handle in the sender |
| Calling process ID | `0x20` | replaces the next word with the sender's process ID |
| Static buffer | type 1; index in bits 10 to 13; size in bits 14 to 31 | copies the data into the buffer the receiver listed for that index |
| Mapped buffer | bit 3; access in bits 1 and 2; size in bits 4 to 31 | lets the receiver read or write the sender's memory in place |

A thread lists the buffers it can receive static data into at 0x180 in its local storage, a descriptor and an address for each of 16 indices. The first normal word of a reply is the result of the command, separate from the result of SendSyncRequest itself, which only says the request reached the service.

## How Hexlith answers

The service and the game share one address space here, so nothing has to be copied between processes. `kernel::IpcRequest` reads the request out of the caller's command buffer, hands the service the words in order with the handles already turned into objects, and writes the service's reply back with the objects it returns turned into handles in the game's table. Mapped buffers are addresses the service reads and writes in the game's memory directly.

A request is answered within the SendSyncRequest that made it, on the calling thread, before that thread runs again. Each service implements `kernel::SessionHandler`, a virtual interface called once per request, and most do it through a table of commands, each with its 3dbrew name and the function that answers it, or none.

## srv:

| Header | Command | Answer |
| --- | --- | --- |
| `0x00010002` | RegisterClient | success, if the request carries the calling process ID descriptor |
| `0x00020000` | EnableNotification | a semaphore srv: releases for each notification it queues, up to 16 |
| `0x00050100` | GetServiceHandle | a session to the named service, if the title may open it |
| `0x00090040` | Subscribe | success; the subscription is remembered |
| `0x000A0040` | Unsubscribe | success |

GetServiceHandle refuses a name longer than eight characters with `0xD9006405`. Its flags say whether to wait for a service that is not registered yet; every service here is registered from the start, so nothing waits. Nothing publishes notifications yet, and ReceiveNotification is not implemented.

## What the reference title sends first

Run from its entry point, the game's first requests are srv:'s RegisterClient and EnableNotification. It then starts a thread at priority `0x18` that waits on the notification semaphore, opens `APT:U`, and sends it command `0x00010040`, GetLockHandle, which is where the run stops until APT is implemented.
