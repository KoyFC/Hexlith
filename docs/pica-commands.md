# The PICA command processor, compared with Azahar

`domain::gpu::PicaState` and `CommandProcessor` read command lists the way [`formats/pica-registers.md`](formats/pica-registers.md) describes. Unit tests check each thing a register does on synthetic lists. They cannot find the mistake nobody thought of, so the processor is also run against Azahar's, the way [`arm-decoder.md`](arm-decoder.md) runs the ARM decoder against Capstone.

## Method

`scripts/compare_pica_commands.py` writes a file of cases that both sides run. A case starts from a known state and is a sequence of steps; a step writes blocks of memory, which is where the lists it jumps to live, and runs one list. Azahar runs the cases through `Pica::PicaCore::ProcessCmdList` at its first vertical blank, under `third_party/azahar/oracle.patch`, with its draws turned into reports. Hexlith runs them through `CommandProcessor` in `tools/hexlith-oracle-run`.

After every step each side reports the events the list caused, in order: each draw, vertex given in immediate mode, primitive restart and FINALIZE, with a hash of the registers at that moment and, for an immediate vertex, of its inputs. It then reports a hash of each section of the state, and the registers themselves:

| | Section |
| --- | --- |
| 0 | The 300h internal registers Azahar keeps |
| 1 to 5 | The vertex shader's code, operand descriptors, float uniforms, bool uniforms and integer uniforms |
| 6 to 10 | The same for the geometry shader |
| 11 | The fixed attributes |
| 12 | The 24 lighting tables |
| 13 | The fog table |
| 14 | The procedural texture tables |
| 15 | The inputs of the last vertex given in immediate mode |

The random lists aim at the registers that do something: uniform, code and descriptor uploads with their indices, fixed attributes and immediate mode, the vertex count and the shader unit modes that decide whether the vertex shader's writes reach the geometry shader, the three kinds of table, draws, restarts, FINALIZE with values that match and do not, jumps to lists in VRAM that may jump on, and writes of any register from 0040h to 03FFh, alone or in runs, under random byte masks.

The recording case is every list of the reference title's recording in order, from boot to frame 1150, each after the pages recorded before it.

## Results

On 2026-10-18:

| Cases | Lists | Events | Agree |
| --- | --- | --- | --- |
| 50,000 random, seed 3 | 99,854 | 246,414 | all |
| 5,000 random, seed 2 | 9,988 | 24,479 | all |
| The recording to frame 1150 | 4,915 | 110,654 | all |

Every list the title wrote leaves both processors in the same state, and asks for the same draws in the same states.

## What the comparison found in Azahar

The first runs did not agree, and every difference turned out to be Azahar's. The random lists now stay away from them, which is why they agree; each is listed here so that it is not taken for a property of the hardware.

| What | Effect in Azahar | What Hexlith does |
| --- | --- | --- |
| A list whose last command is an odd number of words | After the padding word it reads one command past the end of the list, from whatever memory follows | Stops at the end, as GBATEK describes a list |
| `FIXEDATTRIB_INDEX` past 0Fh | `SubmitImmediate` returns before it empties its three-word queue, and the next word overruns it: an assertion, or memory corrupted in a release build | Drops the attribute and empties the queue |
| A lighting table number from 24 to 31 in `LIGHTING_LUT_INDEX` | Indexes past its 24 tables: an assertion | Drops the word |
| `CMDBUF_JUMP` to memory that is not there | Reads through a null pointer | Refuses the list |
| A command that writes one FIFO port or one triggering register several times | Its batch path stores the last value in the register before it handles the first, so a draw or a vertex in the middle sees a later value, and a topology written twice restarts the assembler twice with the second | Each write is stored before it is handled |
| Such a command uploading uniforms with an index that runs out of range | The batch path stops at the first uniform out of range and drops the words after it; its path for single words keeps queueing them | Keeps queueing them, as a FIFO would |
| Such a command uploading code or descriptors past the end of the memory | The batch path writes nothing; the single-word path writes up to the end | Writes up to the end |

The random lists avoid these by ending every list on a two-word command, never writing the fixed attribute index past 0Fh or a lighting table past 23, never jumping except to lists they placed, writing FIFO ports and triggering registers once a command or in consecutive runs, and setting a uniform index before every batch of uniform words that cannot run past the end. Event hashes leave the FIFO data ports out, since what one of them holds in the middle of a command is the artefact above.

Where Azahar and GBATEK read a list differently, the choices are in [`formats/pica-registers.md`](formats/pica-registers.md). The random lists write no register below 0040h but FINALIZE, and leave the IRQ compare and autostop as both sides set them, so that those differences do not show either. They also keep the header's unused bits 28 to 30 clear.

## What is not compared

What a draw then draws: that is the rasterizer's, and is compared on its own. The values of registers 0300h to 03FFh, which Azahar does not keep. What happens when the CPU writes GPU registers directly, which no list does.

## Running it

```sh
scripts/compare_pica_commands.py --azahar ~/opt/src/azahar/build/bin/Release/azahar \
    --image ~/recomp-data/rom/pokemon-x.3ds \
    --tool build/release/tools/hexlith-oracle-run/hexlith-oracle-run \
    --work /tmp/pica --random 50000 --seed 3
scripts/compare_pica_commands.py ... --recording ~/recomp-data/work/0004000000055D00-v00/gpu
```

Fifty thousand random cases take half a minute; the recording, with its 300 MB of pages, five seconds. Run both after any change to `src/domain/gpu`.
