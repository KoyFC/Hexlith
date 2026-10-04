# The ARM and Thumb decoders

`DecodeArm` turns one 32-bit word into a typed instruction, and `DecodeThumb` does the same for one or two halfwords. It is total: every one of the four billion words gets an answer, and the answer for the ones ARMv6K does not define is `Undefined`.

That totality is the point. A pass that has to tell code from data asks this question of data constantly, so a decoder that failed instead of answering would need its failure handled at every step, and a decoder that guessed would plant instructions in the middle of a table.

## Thumb

ARMv6K has Thumb-1: sixteen bits per instruction, except for the branch with link, which is a pair of halfwords and the only four-byte instruction in it. Thumb-2 belongs to ARMv6T2 and the ARM11 does not have it, so a halfword pair that would be a 32-bit Thumb-2 instruction on a later core is not one here. `DecodeThumb` refuses the if-then blocks, the compare-and-branch instructions and the rest of that space, and a disassembler for a later architecture decodes them, which the comparison counts.

Two details in Thumb are easy to get wrong and are both measurable against the image.

**A literal pool read rounds the program counter down to a word.** A Thumb instruction can sit at any even address, so the program counter it reads is not word aligned half the time, and `ldr rd, [pc, #imm]` reads from `Align(pc, 4) + imm`. Getting this wrong reads two bytes into the wrong word for half the instructions in a function.

**A branch with link that exchanges into ARM lands on a word boundary**, and the architecture rounds down to reach one. Hexlith had this wrong until the comparison found 109 branch targets in the text segment where it disagreed, all of them a `blx` at an address that was not a multiple of four.

Of the 65,536 halfwords, 58,852 are instructions, which is 89.8 percent. Thumb leaves far less of its space undefined than ARM does, having sixteen bits to spend rather than thirty-two, which makes "does this decode" even weaker evidence there than in ARM.

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

Current result:

| | ARM, random | ARM, text segment | Thumb, random | Thumb, text segment |
| --- | --- | --- | --- | --- |
| Instructions compared | 5,000,000 | 1,053,596 | 1,000,000 | 2,218,060 |
| Disagreements | 0 | 0 | 0 | 0 |
| Hexlith stricter | 3.7 percent | 0.11 percent | 0.08 percent | 71 words |
| Floating point | 3.4 percent | 5.2 percent | none | none |

The comparison checks more than the operation. Over the text segment read as ARM it agreed on 166,430 branch targets, 308,516 transfer base registers, 298,228 transfer offsets with their direction and write-back, and 52,542 register lists; read as Thumb, on 774,368 branch targets and 180,720 transfer bases. The Thumb sweep is run from offset 0 and from offset 2, so that every halfword position is covered.

Add `--thumb` to compare the Thumb decoder:

```sh
scripts/compare_arm_decoder.py --thumb --random 1000000
scripts/compare_arm_decoder.py --thumb --image <path> --length 0x43BEDC --address 0x00100000
```

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
- In Thumb, `setend` was matched on one bit too few, a linking exchange through the program counter was accepted, and the rounding of a `blx` target was missing.

## Two differences that are expected

Both are counted rather than hidden, so that a real disagreement cannot hide among them.

**Hexlith is stricter.** The architecture fixes some fields to all zeroes or all ones and calls the result unpredictable when a word does not respect them. Hexlith refuses those words. Capstone decodes many of them, because it implements a later architecture in which some of those fields mean something.

This is deliberate and it is not pedantry. A compiler emits those fields correctly without exception, so a word that gets them wrong is far more likely to be data, and this decoder's answer is what the classification pass is built on. The cost is visible: 3.7 percent of random words fall in the gap, against 0.11 percent of the real text segment, and the second number is the one that matters.

**Floating point is identified, not decoded.** VFP is encoded as coprocessor instructions on coprocessors 10 and 11, and that is how they are reported, with the coprocessor number saying which they are. The ARM11 in the 3DS has VFPv2, and there are 55,072 words in the text segment that read as a coprocessor instruction on 10 or 11, so this is not a corner that can stay empty forever. It can stay empty until something needs float semantics.

## Identified rather than decoded

Two places, each because nothing needs the detail yet and the encoding is kept on the instruction, so filling one in later means reading a field rather than finding the instructions again.

- Floating point, as above.
- The lane selectors of the halfword multiplies are recorded on the instruction, but the mnemonic does not spell them.

The thirty-six parallel arithmetic instructions are one operation, `ParallelArithmetic`, with the prefix and the lane shape as two fields beside it. They used to be identified and nothing more; the interpreter needs to know which one it is running, so the fields are filled in and the comparison checks the full mnemonic against Capstone's.

## What the text segment looks like read as ARM

A measurement that decided how classification has to work. Reading the whole 4,439,772-byte text segment as a sequence of ARM words, 1,109,943 in all:

| | Words |
| --- | --- |
| Decode as some ARM instruction | 1,104,025 |
| Do not decode | 5,918 |

**99.5 percent of the segment decodes as valid ARM**, and the segment is not all ARM code. It holds Thumb functions, literal pools in the middle of functions, jump tables and padding, and nearly all of that decodes as some valid ARM instruction as well.

So whether a word decodes is almost no evidence about what it is. Classification cannot be a linear sweep that accepts whatever decodes; it has to be a walk from entry points that are known, following control flow, with the instruction set carried along the path. [`classification.md`](classification.md) is what came of that.

The first four words of the segment are a worked example of why the walk needs the exports: they are four `bl` instructions in a row, the shape of a run of static constructors, and the fifth is a `blx` into Thumb code at 0x00102F00.

## Bit positions

Bit positions are left as numbers rather than named constants, and the `clang-tidy` configuration in `src/domain/arm` turns off the check that objects.

A decoder is written in bit positions, and a bit position is the field specification. `Bits(word, 23, 20)` says which field it reads; a constant named after that field would say it twice and hide it once. The only way this code can be reviewed is by reading a line against the architecture reference, and that requires the bit numbers to be on the line.
