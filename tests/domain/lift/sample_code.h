#ifndef HEXLITH_TESTS_DOMAIN_LIFT_SAMPLE_CODE_H
#define HEXLITH_TESTS_DOMAIN_LIFT_SAMPLE_CODE_H

#include <array>
#include <cstdint>

// A few functions written for the tests of generated code, as ARM machine code
// the test builds itself: a loop, a call, a switch through a jump table, memory
// and block transfers, conditional execution, an instruction the lifter leaves to
// the interpreter and a read of the thread ID register. The build generates C++
// for them (sample_generator.cpp) and the test runs that C++ against the
// interpreter from the same states.
namespace hexlith::domain::lift::test::sample {

constexpr std::uint32_t Base = 0x00100000;
constexpr std::uint32_t Word = 4;

constexpr std::uint32_t At(std::uint32_t index) {
    return Base + (index * Word);
}

// A branch, or a branch with link, from the instruction at one index to the
// instruction at another, under a condition (0xE is always).
constexpr std::uint32_t
Branch(std::uint32_t condition, std::uint32_t from, std::uint32_t to, bool link = false) {
    const std::uint32_t displacement = (to - (from + 2)) & 0x00FFFFFFU;
    return (condition << 28U) | 0x0A000000U | (link ? 0x01000000U : 0U) | displacement;
}

constexpr std::uint32_t Equal = 0x0;
constexpr std::uint32_t Always = 0xE;

// The first instruction of each function.
constexpr std::uint32_t SumIndex = 0;
constexpr std::uint32_t CallerIndex = 8;
constexpr std::uint32_t SwitcherIndex = 18;
constexpr std::uint32_t MixedIndex = 32;
constexpr std::uint32_t PatchedIndex = 44;

// The word of the patched function that a module's loader would have written
// over: the file says one thing and memory another. The generated code is told
// the word is relocated, so the test runs it against memory's version.
constexpr std::uint32_t RelocatedIndex = PatchedIndex + 1;
constexpr std::uint32_t RelocatedFileWord = 0xE3A00002;    // mov r0, #2
constexpr std::uint32_t RelocatedMemoryWord = 0xE3A00007;  // mov r0, #7

// Where the jump table of the switcher is, and how many entries it holds.
constexpr std::uint32_t TableIndex = 21;
constexpr std::uint32_t TableEntries = 3;

// A supervisor call, at the address the functions are given as their return
// address so that a run ends there.
constexpr std::uint32_t SentinelIndex = 64;

constexpr std::array<std::uint32_t, SentinelIndex + 1> Code = [] {
    std::array<std::uint32_t, SentinelIndex + 1> words{};
    constexpr std::uint32_t Nop = 0xE320F000;
    for (std::uint32_t& word : words) {
        word = Nop;
    }

    // sum: r0 = 1 + 2 + ... + r1, by a loop.
    words[0] = 0xE3A02000;            // mov r2, #0
    words[1] = 0xE3510000;            // cmp r1, #0
    words[2] = Branch(Equal, 2, 6);   // beq the end
    words[3] = 0xE0822001;            // add r2, r2, r1
    words[4] = 0xE2511001;            // subs r1, r1, #1
    words[5] = Branch(Always, 5, 1);  // b the test
    words[6] = 0xE1A00002;            // mov r0, r2
    words[7] = 0xE12FFF1E;            // bx lr

    // caller: sums 1 to 10, and stores and reloads it through r3.
    words[8] = 0xE92D4010;                    // push {r4, lr}
    words[9] = 0xE3A0100A;                    // mov r1, #10
    words[10] = Branch(Always, 10, 0, true);  // bl sum
    words[11] = 0xE1A04000;                   // mov r4, r0
    words[12] = 0xE5834004;                   // str r4, [r3, #4]
    words[13] = 0xE5930004;                   // ldr r0, [r3, #4]
    words[14] = 0xE1500004;                   // cmp r0, r4
    words[15] = 0x02800001;                   // addeq r0, r0, #1
    words[16] = 0x13A00000;                   // movne r0, #0
    words[17] = 0xE8BD8010;                   // pop {r4, pc}

    // switcher: r0 becomes 100, 200 or 300 for an index of 0, 1 or 2, and 0
    // for anything else.
    words[18] = 0xE3500002;              // cmp r0, #2
    words[19] = 0x979FF100;              // ldrls pc, [pc, r0, lsl #2]
    words[20] = Branch(Always, 20, 30);  // b the default
    words[21] = At(24);                  // case 0
    words[22] = At(26);                  // case 1
    words[23] = At(28);                  // case 2
    words[24] = 0xE3A00064;              // mov r0, #100
    words[25] = 0xE12FFF1E;              // bx lr
    words[26] = 0xE3A000C8;              // mov r0, #200
    words[27] = 0xE12FFF1E;              // bx lr
    words[28] = 0xE3A00F4B;              // mov r0, #300
    words[29] = 0xE12FFF1E;              // bx lr
    words[30] = 0xE3A00000;              // mov r0, #0
    words[31] = 0xE12FFF1E;              // bx lr

    // mixed: a call, block transfers, an instruction left to the interpreter and
    // a read of the thread ID register.
    words[32] = 0xE92D4030;                    // push {r4, r5, lr}
    words[33] = 0xE1A04000;                    // mov r4, r0
    words[34] = Branch(Always, 34, 18, true);  // bl switcher
    words[35] = 0xE0800004;                    // add r0, r0, r4
    words[36] = 0xE8830003;                    // stm r3, {r0, r1}
    words[37] = 0xEE300A01;                    // vadd.f32 s0, s0, s2
    words[38] = 0xEE1D5F70;                    // mrc p15, 0, r5, c13, c0, 3
    words[39] = 0xE1B01180;                    // lsls r1, r0, #3
    words[40] = 0xE8930006;                    // ldm r3, {r1, r2}
    words[41] = 0xE0800001;                    // add r0, r0, r1
    words[42] = 0xE8BD8030;                    // pop {r4, r5, pc}

    // patched: r0 is 1, then whatever the relocated word makes it, then 10 more.
    words[44] = 0xE3A00001;         // mov r0, #1
    words[45] = RelocatedFileWord;  // mov r0, #2 in the file
    words[46] = 0xE280000A;         // add r0, r0, #10
    words[47] = 0xE12FFF1E;         // bx lr

    words[SentinelIndex] = 0xEF000000;  // svc #0
    return words;
}();

}  // namespace hexlith::domain::lift::test::sample

#endif  // HEXLITH_TESTS_DOMAIN_LIFT_SAMPLE_CODE_H
