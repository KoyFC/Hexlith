# The ARM decoder

`DecodeArm` turns one 32-bit word into a typed instruction. It is total: every one of the four billion words gets an answer, and the answer for the ones ARMv6K does not define is `Undefined`.

That totality is the point. A pass that has to tell code from data asks this question of data constantly, so a decoder that failed instead of answering would need its failure handled at every step, and a decoder that guessed would plant instructions in the middle of a table.

## How it is verified

Unit tests cover the encodings someone thought to write down. The mistakes that matter in a decoder are the ones nobody thought of: a table row placed one position off, a field read from the wrong end, a whole group refused.

So the decoder is compared against Capstone, over the reference image and over random words.

```sh
cmake --build --preset ci
scripts/compare_arm_decoder.py --random 1000000
scripts/compare_arm_decoder.py --image ~/recomp-data/work/<id>/exefs/.code \
    --length 0x43BEDC --address 0x00100000
```

`hexlith-arm-dump` prints one record per word and the script checks each one. Capstone is a development dependency and nothing else: the build does not use it, the tests do not need it, and the decoder does not depend on it.

Current result, five million random words and the whole text segment:

| | Random | Text segment |
| --- | --- | --- |
| Words compared | 5,000,000 | 1,053,596 |
| Disagreements | 0 | 0 |
| Hexlith stricter | 3.7 percent | 0.11 percent |
| Floating point | 3.4 percent | 5.2 percent |

The comparison checks more than the operation. Over the text segment it agreed on 166,430 branch targets, 308,516 transfer base registers, 298,228 transfer offsets with their direction and write-back, and 52,542 register lists.

## What the comparison found

Worth recording, because it is the argument for the method. Each of these was in the decoder when its unit tests passed:

- The two-register coprocessor transfer was tested at the wrong bit offset, so every `mcrr` and `mrrc` was read as a `stc` or an `ldc` and the other way round.
- The sum of absolute differences was placed in the signed multiply quarter of the media space. It is in the quarter after it, and the row it was given belongs to nothing.
- Half the packing and saturation table was mapped to the wrong rows, which produced a `pkhbt` where a `ssat` or a `usat` belonged.
- The five hint instructions were refused outright, because the space they live in is a write to the status register that writes no field of it. There are 16,282 of them in the text segment, which is what a compiler leaves behind when it aligns code.
- `sel` was decoded as though it took one register. It takes two.
- `clrex` and `smc` were not decoded at all.
- `umaal` accepted a flag-setting bit it does not have, and a breakpoint accepted a condition it cannot carry.
- A coprocessor transfer accepted an addressing mode that does not exist.

## Two differences that are expected

Both are counted rather than hidden, so that a real disagreement cannot hide among them.

**Hexlith is stricter.** The architecture fixes some fields to all zeroes or all ones and calls the result unpredictable when a word does not respect them. Hexlith refuses those words. Capstone decodes many of them, because it implements a later architecture in which some of those fields mean something.

This is deliberate and it is not pedantry. A compiler emits those fields correctly without exception, so a word that gets them wrong is far more likely to be data, and this decoder's answer is what the classification pass is built on. The cost is visible: 3.7 percent of random words fall in the gap, against 0.11 percent of the real text segment, and the second number is the one that matters.

**Floating point is identified, not decoded.** VFP is encoded as coprocessor instructions on coprocessors 10 and 11, and that is how they are reported, with the coprocessor number saying which they are. The ARM11 in the 3DS has VFPv2, and there are 55,072 words in the text segment that read as a coprocessor instruction on 10 or 11, so this is not a corner that can stay empty forever. It can stay empty until something needs float semantics.

## Identified rather than decoded

Three places, each because nothing needs the detail yet and the encoding is kept on the instruction, so filling one in later means reading a field rather than finding the instructions again.

- Floating point, as above.
- The forty-eight parallel arithmetic encodings, reported as `ParallelArithmetic`. Six prefixes over eight lane shapes, all doing one thing to different widths.
- The lane selectors of the halfword multiplies are recorded on the instruction, but the mnemonic does not spell them.

## What the text segment looks like read as ARM

A measurement that decided how classification has to work. Reading the whole 4,439,772-byte text segment as a sequence of ARM words, 1,109,943 in all:

| | Words |
| --- | --- |
| Decode as some ARM instruction | 1,104,025 |
| Do not decode | 5,918 |

**99.5 percent of the segment decodes as valid ARM**, and most of the segment is Thumb. The export table of the static module says so: 338 of its 341 code exports are Thumb entry points.

So whether a word decodes is almost no evidence about what it is. Classification cannot be a linear sweep that accepts whatever decodes; it has to be a walk from entry points that are known, following control flow, with the instruction set carried along the path. That is what the next stage does.

The first four words of the segment are a worked example of why the walk needs the exports: they are four `bl` instructions in a row, the shape of a run of static constructors, and the fifth is a `blx` into Thumb code at 0x00102F00.

## Bit positions

Bit positions are left as numbers rather than named constants, and the `clang-tidy` configuration in `src/domain/arm` turns off the check that objects.

A decoder is written in bit positions, and a bit position is the field specification. `Bits(word, 23, 20)` says which field it reads; a constant named after that field would say it twice and hide it once. The only way this code can be reviewed is by reading a line against the architecture reference, and that requires the bit numbers to be on the line.
