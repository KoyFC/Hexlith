# Golden traces

A golden trace is the sequence of things a run does that another emulator can be asked to do too: the requests it sends to services and the CRO modules it loads and unloads, one line each, with the frame it happened in. Two runs of the same boot, one in Hexlith and one in Azahar, are compared line by line, and the first place they disagree is where to look.

This is the first version of the differential testing `AGENTS.md` plans. It compares sequences and not times. Hexlith's virtual clock is not Azahar's, and the two do not take the same number of frames to do the same thing, so the frame is reported and never compared.

## The format

```
# hexlith golden trace, format 1
0 ipc srv: 00010002
5 cro-load DllLangSelect 006A5000
225 cro-unload DllLangSelect 006A5000
```

The first line names the format. Every other line is a frame, the count of the top screen's vertical blanks since the process started, then an event:

| Event | Fields | When |
| --- | --- | --- |
| `ipc` | the service, the request's header | Once the service has answered the request |
| `cro-load` | the module's name, its address | Once ldr:ro has mapped and linked it, before the reply |
| `cro-unload` | the module's name, its address | Once ldr:ro has unmapped it, before the reply |

A session that is not a named service has a name of its own: `fs:file` for a file opened on fs:USER and `fs:directory` for a directory.

## Writing one

`hexlith-boot --golden <path>` writes Hexlith's.

Azahar writes its own into its log once `third_party/azahar/golden-trace.patch` is applied, as lines containing `golden ipc`, `golden cro-load`, `golden cro-unload` and `golden vblank`. `third_party/azahar/README.md` has the build. `scripts/compare_golden_trace.py extract <log>` turns the log into a golden trace, counting the vertical blanks for the frame.

## Comparing

```sh
scripts/compare_golden_trace.py compare hexlith.txt azahar.txt \
    --until 'cro-load DllSequence 006D1000' [--per-service]
```

`--until` cuts both traces after the first event that reads the same, so that a run stopped at a moment is compared with the same stretch of a run that went on. By default a run of identical consecutive events counts as one, since how many times a thread polls before something happens is timing. The comparison prints the first divergence with the events before it, and the frame of every module event on both sides.

The two emulators do not interleave their threads the same way, so the sequence of all services together usually parts early for reasons that mean nothing. `--per-service` compares each service's requests on their own, which is where a divergence that matters shows.

Two things about Azahar's side are worth knowing. The patch logs a request once its handler has run, so a handler that answers later, as some of Azahar's file system handlers do, logs out of order with the other threads. And a request a handler declines is logged as well, as Hexlith logs one it does not implement.

## The first comparison

On 2026-10-17 both emulators booted a new game with two presses of A through the language selection, Hexlith from the input script `100 tap a`, `200 tap a`, Azahar from a person. Cut at the load of `DllSequence`, the last of the title screen's modules:

| Service | Events, Hexlith | Events, Azahar | Result |
| --- | --- | --- | --- |
| `srv:`, `APT:U`, `ndm:u`, `cfg:u`, `hid:USER`, `dsp::DSP`, `frd:u`, `boss:U`, `ptm:u` | 66 | 66 | The same |
| `ldr:ro` | 11 | 11 | The same |
| `fs:directory` | 2 | 2 | The same |
| `gsp::Gpu` | 459 | 645 | The same for all of Hexlith's 459; Azahar draws more frames |
| `fs:USER` | 28 | 33 | Parts at event 6 |
| `fs:file` | 7 | 13 | Parts at event 3 |

The modules load and unload in the same order at the same addresses:

| Event | Frame, Hexlith | Frame, Azahar |
| --- | --- | --- |
| `cro-load DllLangSelect 006A5000` | 5 | 115 |
| `cro-unload DllLangSelect 006A5000` | 225 | 432 |
| `cro-load DllTitle 006A5000` | 227 | 448 |
| `cro-load DllBackGround 006C8000` | 227 | 449 |
| `cro-load DllSequence 006D1000` | 227 | 450 |

**The divergence.** Every time the game opens its RomFS again after the first two times, Azahar shows a `GetPriority` on fs:USER right after the `OpenFileDirectly`, at frames 37, 94, 110, 442 and 460, and later a `Close` on the file session. Hexlith shows neither. Everything else on fs:USER is the same, the formatting of the save data and the creation of the extra data included, 25 requests in the same order. The game takes a different path through its file reading, and nothing yet says why.

**The frames.** Azahar takes 115 frames to load the first module and Hexlith 5, and 16 frames from the unload to the title screen against 2. Azahar makes a file system request wait: every read and every open sleeps the calling thread for a time measured on hardware (`DelayGenerator`s in `src/core/file_sys/`), and Hexlith answers at once. The boot reads its RomFS about a thousand times before the first module. That would account for much of the difference and perhaps for the divergence above, if the game's file code behaves differently when its reads take time. Neither has been checked.
