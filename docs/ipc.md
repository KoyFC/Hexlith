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

## Declining a request

Some commands are implemented for the requests the game is known to send and not for every request they can carry: cfg:u holds the configuration blocks Azahar creates a new console with, and the console holds more. A block outside the table is not answered as missing, since a game told something is missing may take a path it never takes on a console. The service declines the request instead, `kernel::IpcRequest::Decline` with the reason, and the run stops as it does at a command with no implementation, with the reason after the service's name.

## The services the boot reaches

Each answers from 3dbrew's description of the command and the answer Azahar gives, cited in the service's header. What none of them does yet is noted.

| Service | Class | What it answers | What it does not do |
| --- | --- | --- | --- |
| `APT:U` | `AppletManager` | The lock, the notification and parameter events, the Wakeup parameter the first applet gets, the applet utility, the shared font | No other applet runs, so no parameter is ever sent but the first |
| `ndm:u` | `NetworkDaemonManager` | Which daemons may run, and a count of suspensions for each | No daemon runs |
| `fs:USER` | `FileSystem`, `OpenFileSession` | The title's RomFS, read from level 3 of its IVFC tree; save data that is not formatted; every other archive not found | No writes, no directories, no save data that persists |
| `cfg:u` | `Configuration` | The blocks of a new console's configuration, the region, the model, the transferable ID | Blocks outside the table are declined |
| `hid:USER` | `HumanInterface` | The shared page and its five events, the gyroscope's calibration, the volume | Nothing fills the page: no button is ever held |
| `gsp::Gpu` | `Graphics` | The right to the GPU, registers that keep what is written, the command queue, interrupts and both vertical blanks every 4,481,136 ticks | Nothing is drawn; a command completes at once |
| `dsp::DSP` | `DigitalSignalProcessor` | The firmware load, the interrupt events, the audio pipe's start-up reply with the structure addresses, an audio frame every 1,310,720 ticks | No firmware runs and no sound is mixed |
| `frd:u` | `Friends` | An offline console with a zero friend key and no friends | |
| `boss:U` | `SpotPass` | A session, and storage accepted and forgotten | Nothing is ever downloaded |
| `ptm:u` | `PowerManager` | A step history of zero steps an hour | |
| `ldr:ro` | `RelocatableObjects` | The CRS, the CRR, and CRO modules loaded, linked and fixed as the console's loader leaves them | Unloading, LoadCRO_New, linking by hand. [`cro-linking.md`](cro-linking.md) has the rest |

The shared font is a stand-in built in code from the format, since the console's is not on the cartridge and the game stops without one. [ADR 0014](adr/0014-a-stand-in-for-the-shared-font.md) records why.

## What the reference title sends first

Run from its entry point, the game's first requests are srv:'s RegisterClient and EnableNotification. It then opens `APT:U`, takes its lock, registers, waits for the Wakeup parameter and enables itself, and suspends the daemons it does not want running. It initialises `fs:USER` and opens its RomFS, reads its configuration and its input page, maps the shared font, takes the GPU and sets up its interrupt queue, loads the DSP firmware and starts the audio pipe, and asks `frd:u` and `boss:U` about the network. After 456 requests and about 8.95 million ticks it opens `ldr:ro` and sends Initialize, `0x000100C2`, with the CRS, then LoadCRR. It reads more of its RomFS, asks `ptm:u` for a day of steps, and at about 23.3 million ticks sends LoadCRO for `DllLangSelect`, the language selection that a first boot shows. The module runs, and the boot carries on until the game formats its save data, at about 67 million ticks.
