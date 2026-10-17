#include "core/byte_reader.h"
#include "domain/ctr/cro.h"
#include "domain/ctr/layout.h"
#include "domain/services/relocatable_objects.h"

#include "service_harness.h"
#include "support/synthetic_cro.h"

#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace hexlith::domain::services {
namespace {

namespace cro = ctr::cro;
namespace ipc = kernel::ipc;
using ctr::CroSegmentType;
using test::AnonymousImport;
using test::BuildSyntheticCro;
using test::ImportsFrom;
using test::SyntheticCroRelocation;
using test::SyntheticCroSegment;
using test::SyntheticCroSpec;
using testing::ServiceHarness;

// ldr:ro command headers. Source: 3dbrew, RO Services.
constexpr std::uint32_t Initialize = 0x000100C2;
constexpr std::uint32_t LoadCro = 0x000402C2;
constexpr std::uint32_t CurrentProcess = 0xFFFF8001;

constexpr std::uint32_t Page = kernel::layout::PageSize;
constexpr std::uint32_t Code = 0;
constexpr std::uint32_t Data = 2;

// The main executable's text, which the static module describes, and places
// in it: the hook unresolved imports point at, and a word the static module
// patches with a function a module exports.
constexpr std::uint32_t StaticText = 0x00100000;
constexpr std::uint32_t UnresolvedOffset = 0x800;
constexpr std::uint32_t StaticPatchedWord = 0x200;
constexpr std::uint32_t StaticFunction = 0x101;

// Where the game keeps the files it read, and where it maps them.
constexpr std::uint32_t HeapBuffers = 0x08000000;
constexpr std::uint32_t CrsAddress = 0x01000000;
constexpr std::uint32_t ModuleA = 0x006A5000;
constexpr std::uint32_t ModuleB = 0x006B0000;
constexpr std::uint32_t FixLevel = 1;

// Words module A patches, and what module B exports to the static module.
constexpr std::uint32_t StaticImportWord = 0x10;
constexpr std::uint32_t DataPointerWord = 4;
constexpr std::uint32_t DataPointerTarget = 16;
constexpr std::uint32_t FunctionInB = 0x50;

std::uint32_t Tag(std::uint32_t segment, std::uint32_t offset) {
    return (offset << cro::SegmentTagOffsetShift) | segment;
}

SyntheticCroRelocation Patch(std::uint32_t segment,
                             std::uint32_t offset,
                             std::uint32_t addend = 0,
                             std::uint8_t symbol_segment = 0) {
    return SyntheticCroRelocation{.segment_index = segment,
                                  .offset_in_segment = offset,
                                  .addend = addend,
                                  .symbol_segment = symbol_segment};
}

SyntheticCroSpec StaticSpec() {
    SyntheticCroSpec spec;
    spec.static_module = true;
    spec.segments = {SyntheticCroSegment{
        .type = CroSegmentType::Code, .size = Page, .virtual_address = StaticText}};
    spec.import_modules = {
        ImportsFrom("DllB", {AnonymousImport(Code, FunctionInB, {Patch(Code, StaticPatchedWord)})}),
    };
    spec.on_unresolved_hook = Tag(Code, UnresolvedOffset);
    return spec;
}

// The four segments of a small module.
constexpr std::size_t CodeSize = 0x100;
constexpr std::size_t ReadOnlySize = 0x40;
constexpr std::size_t DataSize = 0x20;
constexpr std::size_t BssSize = 0x40;

std::vector<SyntheticCroSegment> FourSegments() {
    return {
        SyntheticCroSegment{.type = CroSegmentType::Code, .size = CodeSize},
        SyntheticCroSegment{.type = CroSegmentType::ReadOnlyData, .size = ReadOnlySize},
        SyntheticCroSegment{.type = CroSegmentType::Data, .size = DataSize},
        SyntheticCroSegment{.type = CroSegmentType::Bss, .size = BssSize},
    };
}

SyntheticCroSpec ModuleASpec() {
    SyntheticCroSpec spec;
    spec.name = "DllA";
    spec.segments = FourSegments();
    spec.import_modules = {
        ImportsFrom("|static|",
                    {AnonymousImport(Code, StaticFunction, {Patch(Code, StaticImportWord)})}),
    };
    spec.internal_relocations = {Patch(Data, DataPointerWord, DataPointerTarget, Data)};
    return spec;
}

SyntheticCroSpec ModuleBSpec() {
    SyntheticCroSpec spec;
    spec.name = "DllB";
    spec.segments = FourSegments();
    return spec;
}

// A file as the game leaves it in its buffer: a whole number of pages.
std::vector<std::uint8_t> PageAligned(std::vector<std::uint8_t> file) {
    file.resize((file.size() + Page - 1) / Page * Page, 0);
    return file;
}

struct Harness {
    Harness() : service(harness.Kernel()) {
        kernel::AddressSpace& space = harness.Kernel().Space();
        EXPECT_EQ(space.CommitFixed(
                      StaticText, Page, memory::Access::ReadExecute, kernel::MemoryState::Code),
                  kernel::result::Success);
        EXPECT_EQ(space.CommitHeap(HeapBuffers, 16 * Page, memory::Access::ReadWrite),
                  kernel::result::Success);
    }

    // Copies a file into the next free pages of the heap, as the game does
    // after reading it from its RomFS, and answers where.
    std::uint32_t Buffer(const std::vector<std::uint8_t>& file) {
        const std::uint32_t at = next_buffer;
        harness.WriteBytes(at, file);
        next_buffer += static_cast<std::uint32_t>(file.size());
        return at;
    }

    // Two pages of heap for a module's data and BSS.
    std::uint32_t Buffers() {
        const std::uint32_t at = next_buffer;
        next_buffer += 2 * Page;
        return at;
    }

    bool InitializeWith(const std::vector<std::uint8_t>& file, std::uint32_t address) {
        const std::uint32_t buffer = Buffer(file);
        return harness.Send(service,
                            {Initialize,
                             buffer,
                             static_cast<std::uint32_t>(file.size()),
                             address,
                             ipc::CopyHandlesDescriptor(1),
                             CurrentProcess});
    }

    bool Load(const std::vector<std::uint8_t>& file, std::uint32_t address) {
        const std::uint32_t buffer = Buffer(file);
        const std::uint32_t data = Buffers();
        return harness.Send(service,
                            {LoadCro,
                             buffer,
                             address,
                             static_cast<std::uint32_t>(file.size()),
                             data,
                             0,
                             Page,
                             data + Page,
                             Page,
                             1,
                             FixLevel,
                             0,
                             ipc::CopyHandlesDescriptor(1),
                             CurrentProcess});
    }

    std::uint32_t HeaderField(std::uint32_t base, std::size_t field) {
        return harness.Read32(base + static_cast<std::uint32_t>(cro::FieldOffset(field)));
    }

    ServiceHarness harness;
    RelocatableObjects service;
    std::uint32_t next_buffer = HeapBuffers;
    std::vector<std::uint8_t> crs = PageAligned(BuildSyntheticCro(StaticSpec()));
    std::vector<std::uint8_t> a = PageAligned(BuildSyntheticCro(ModuleASpec()));
    std::vector<std::uint8_t> b = PageAligned(BuildSyntheticCro(ModuleBSpec()));
};

std::uint32_t SegmentOffset(const std::vector<std::uint8_t>& file, std::uint32_t segment) {
    const core::Result<ctr::CroHeader> header = ctr::ParseCroHeader(file);
    EXPECT_TRUE(header.Ok());
    return header.Ok() ? static_cast<std::uint32_t>(header.Value().segments.at(segment).offset) : 0;
}

TEST(RelocatableObjects, MapsTheStaticModuleAndPointsItsImportsAtItsHook) {
    Harness ro;
    const std::uint32_t buffer = ro.next_buffer;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    EXPECT_EQ(ro.harness.Reply(1), kernel::result::Success.raw);
    EXPECT_EQ(ro.harness.Read32(StaticText + StaticPatchedWord), StaticText + UnresolvedOffset);
    EXPECT_EQ(ro.harness.Kernel().Space().Memory().AccessAt(buffer), memory::Access::None)
        << "the buffer is closed to the game while the module is mapped from it";
    const std::uint32_t name =
        core::ReadU32At(ro.crs, cro::FieldOffset(cro::ModuleNameOffsetField)).ValueOr(0);
    EXPECT_EQ(ro.HeaderField(CrsAddress, cro::ModuleNameOffsetField), CrsAddress + name)
        << "the header is rebased";
}

TEST(RelocatableObjects, LoadsAModuleLinkedToTheStaticOne) {
    Harness ro;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    ASSERT_TRUE(ro.Load(ro.a, ModuleA));
    EXPECT_EQ(ro.harness.Reply(0), ipc::MakeHeader(0x0004, 2, 0));
    EXPECT_EQ(ro.harness.Reply(1), kernel::result::Success.raw);
    const std::uint32_t fixed_size = ro.harness.Reply(2);
    EXPECT_EQ(fixed_size % Page, 0U);
    EXPECT_LE(fixed_size, ro.a.size());

    const std::uint32_t code = ModuleA + SegmentOffset(ro.a, Code);
    EXPECT_EQ(ro.harness.Read32(code + StaticImportWord), StaticText + StaticFunction);
    EXPECT_EQ(ro.harness.Kernel().Space().Memory().AccessAt(code), memory::Access::ReadExecute);
    EXPECT_TRUE(core::MatchesAt(ro.harness.ReadBytes(ModuleA, cro::HeaderSize),
                                cro::FieldOffset(cro::MagicField),
                                cro::FixedUpMagic));
}

TEST(RelocatableObjects, SaysWhichModuleItLoadedAndWhere) {
    Harness ro;
    std::vector<std::string> events;
    ro.service.OnModuleEvent([&events](const RelocatableObjects::ModuleEvent& event) {
        const bool loaded = event.kind == RelocatableObjects::ModuleEvent::Kind::Loaded;
        events.push_back(
            std::format("{} {} {:08X}", loaded ? "load" : "unload", event.name, event.address));
    });
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    ASSERT_TRUE(ro.Load(ro.a, ModuleA));
    EXPECT_EQ(events, std::vector<std::string>{std::format("load DllA {:08X}", ModuleA)})
        << "the static module is not loaded by LoadCRO";
}

TEST(RelocatableObjects, PatchesDataInTheFileBeforeTheGameCopiesIt) {
    Harness ro;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    const std::uint32_t data_buffer = ro.next_buffer + static_cast<std::uint32_t>(ro.a.size());
    ASSERT_TRUE(ro.Load(ro.a, ModuleA));
    EXPECT_EQ(ro.harness.Read32(ModuleA + SegmentOffset(ro.a, Data) + DataPointerWord),
              data_buffer + DataPointerTarget);
}

TEST(RelocatableObjects, KeepsTheModulesInAListInTheirHeaders) {
    Harness ro;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    ASSERT_TRUE(ro.Load(ro.a, ModuleA));
    EXPECT_EQ(ro.HeaderField(CrsAddress, cro::NextModuleField), ModuleA) << "the head";
    EXPECT_EQ(ro.HeaderField(ModuleA, cro::PreviousModuleField), ModuleA) << "the head is the tail";
    EXPECT_EQ(ro.HeaderField(ModuleA, cro::NextModuleField), 0U);

    ASSERT_TRUE(ro.Load(ro.b, ModuleB));
    EXPECT_EQ(ro.HeaderField(ModuleA, cro::NextModuleField), ModuleB);
    EXPECT_EQ(ro.HeaderField(ModuleA, cro::PreviousModuleField), ModuleB)
        << "the head points at the new tail";
    EXPECT_EQ(ro.HeaderField(ModuleB, cro::PreviousModuleField), ModuleA);
    EXPECT_EQ(ro.HeaderField(ModuleB, cro::NextModuleField), 0U);
}

TEST(RelocatableObjects, HandsTheStaticModuleWhatALaterModuleExports) {
    Harness ro;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    ASSERT_TRUE(ro.Load(ro.a, ModuleA));
    ASSERT_TRUE(ro.Load(ro.b, ModuleB));
    EXPECT_EQ(ro.harness.Read32(StaticText + StaticPatchedWord),
              ModuleB + SegmentOffset(ro.b, Code) + FunctionInB);
    const std::uint32_t batch = ro.HeaderField(CrsAddress, cro::ExternalRelocationTableOffsetField);
    EXPECT_EQ(ro.harness.ReadBytes(batch + cro::RelocationBatchResolvedOffset, 1),
              std::vector<std::uint8_t>{1})
        << "the batch is marked resolved";
}

TEST(RelocatableObjects, RefusesToLoadBeforeItIsInitialized) {
    Harness ro;
    ASSERT_TRUE(ro.Load(ro.a, ModuleA));
    EXPECT_EQ(ro.harness.Reply(1), RelocatableObjects::NotInitialized.raw);
}

TEST(RelocatableObjects, RefusesASecondInitialize) {
    Harness ro;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress + 0x100000));
    EXPECT_EQ(ro.harness.Reply(1), RelocatableObjects::AlreadyInitialized.raw);
}

TEST(RelocatableObjects, RefusesAMisalignedAddress) {
    Harness ro;
    ASSERT_TRUE(ro.InitializeWith(ro.crs, CrsAddress));
    ASSERT_TRUE(ro.Load(ro.a, ModuleA + 4));
    EXPECT_EQ(ro.harness.Reply(1), RelocatableObjects::MisalignedAddress.raw);
}

}  // namespace
}  // namespace hexlith::domain::services
