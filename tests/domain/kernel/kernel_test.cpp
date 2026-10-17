#include "domain/kernel/kernel.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace hexlith::domain::kernel {
namespace {

using memory::Access;

constexpr std::uint32_t Page = layout::PageSize;
constexpr std::uint32_t CodeAddress = 0x00100000;
constexpr std::uint32_t DataAddress = 0x00200000;
constexpr std::uint32_t ChildStackTop = DataAddress + Page;
constexpr std::uint32_t StackSize = 4 * Page;
constexpr std::uint32_t RegionSize = 64 * Page;

constexpr std::uint32_t MainPriority = 0x30;
constexpr std::uint32_t HigherPriority = 0x20;
constexpr std::uint32_t LowerPriority = 0x38;
constexpr std::uint32_t RunFast = 0x03000000;
constexpr std::uint64_t Budget = 100000;

// Offsets in the data page the programs leave their results at.
constexpr std::uint32_t FirstSlot = 0;
constexpr std::uint32_t SecondSlot = 4;
constexpr std::uint32_t ThirdSlot = 8;
constexpr std::uint32_t FourthSlot = 12;

// What a slot holds if nothing was stored in it.
constexpr std::uint32_t Unwritten = 0xDEADBEEF;

// System call numbers. Source: 3dbrew, SVC.
constexpr std::uint32_t ControlMemoryCall = 0x01;
constexpr std::uint32_t ExitProcessCall = 0x03;
constexpr std::uint32_t CreateThreadCall = 0x08;
constexpr std::uint32_t ExitThreadCall = 0x09;
constexpr std::uint32_t SleepThreadCall = 0x0A;
constexpr std::uint32_t CreateMutexCall = 0x13;
constexpr std::uint32_t ReleaseMutexCall = 0x14;
constexpr std::uint32_t CreateEventCall = 0x17;
constexpr std::uint32_t SignalEventCall = 0x18;
constexpr std::uint32_t CreateAddressArbiterCall = 0x21;
constexpr std::uint32_t ArbitrateAddressCall = 0x22;
constexpr std::uint32_t WaitSynchronization1Call = 0x24;
constexpr std::uint32_t GetSystemTickCall = 0x28;
constexpr std::uint32_t ConnectToPortCall = 0x2D;
constexpr std::uint32_t SendSyncRequest1Call = 0x2E;
constexpr std::uint32_t SendSyncRequestCall = 0x32;

// Registers by name, for reading the programs.
constexpr unsigned R0 = 0;
constexpr unsigned R1 = 1;
constexpr unsigned R2 = 2;
constexpr unsigned R3 = 3;
constexpr unsigned R4 = 4;
constexpr unsigned R5 = 5;
constexpr unsigned R6 = 6;

// The ARM encodings the programs below need, in the ARMv6 reference manual's
// forms. The programs' results check them.
constexpr std::uint32_t AlwaysCondition = 0xE0000000;
constexpr unsigned DestinationShift = 12;
constexpr unsigned BaseShift = 16;

// mov rd, #value, for a value under 256
constexpr std::uint32_t MoveImmediate(unsigned destination, std::uint32_t value) {
    constexpr std::uint32_t Opcode = 0x03A00000;
    return AlwaysCondition | Opcode | (destination << DestinationShift) | value;
}

// mvn rd, #value, which makes -1 from 0 and -2 from 1
constexpr std::uint32_t MoveNot(unsigned destination, std::uint32_t value) {
    constexpr std::uint32_t Opcode = 0x03E00000;
    return AlwaysCondition | Opcode | (destination << DestinationShift) | value;
}

// str rt, [rn, #offset]
constexpr std::uint32_t Store(unsigned source, unsigned base, std::uint32_t offset) {
    constexpr std::uint32_t Opcode = 0x05800000;
    return AlwaysCondition | Opcode | (base << BaseShift) | (source << DestinationShift) | offset;
}

// ldr rt, [rn, #offset]
constexpr std::uint32_t Load(unsigned destination, unsigned base, std::uint32_t offset) {
    constexpr std::uint32_t Opcode = 0x05900000;
    return AlwaysCondition | Opcode | (base << BaseShift) | (destination << DestinationShift) |
           offset;
}

// svc #number
constexpr std::uint32_t SupervisorCall(std::uint32_t number) {
    constexpr std::uint32_t Opcode = 0x0F000000;
    return AlwaysCondition | Opcode | number;
}

// A program as ARM words, with constants and the addresses of labels loaded
// from a literal pool placed after the code.
class Program {
public:
    using Label = std::size_t;

    Label NewLabel() {
        labels_.push_back(0);
        return labels_.size() - 1;
    }

    Program& Bind(Label label) {
        labels_.at(label) = words_.size();
        return *this;
    }

    Program& Emit(std::uint32_t word) {
        words_.push_back(word);
        return *this;
    }

    // ldr rd, =value
    Program& Constant(unsigned destination, std::uint32_t value) {
        pool_.push_back(PoolEntry{.load = words_.size(), .value = value});
        return EmitLoadFromPool(destination);
    }

    // ldr rd, =label
    Program& AddressOf(unsigned destination, Label label) {
        pool_.push_back(PoolEntry{.load = words_.size(), .label = label, .is_label = true});
        return EmitLoadFromPool(destination);
    }

    Program& Call(std::uint32_t number) {
        return Emit(SupervisorCall(number));
    }

    [[nodiscard]] std::vector<std::uint8_t> Bytes() const {
        std::vector<std::uint32_t> words = words_;
        // The pc reads two words ahead of the instruction.
        constexpr std::size_t PipelineWords = 2;
        for (const PoolEntry& entry : pool_) {
            const std::size_t slot = words.size();
            words.push_back(entry.is_label ? AddressOfWord(labels_.at(entry.label)) : entry.value);
            words.at(entry.load) |=
                static_cast<std::uint32_t>((slot - entry.load - PipelineWords) * 4);
        }
        std::vector<std::uint8_t> bytes;
        for (const std::uint32_t word : words) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>(word >> shift));
            }
        }
        return bytes;
    }

    // The address of the svc instruction emitted at a given word.
    static std::uint32_t AddressOfWord(std::size_t index) {
        return CodeAddress + static_cast<std::uint32_t>(index * 4);
    }

    [[nodiscard]] std::size_t Size() const {
        return words_.size();
    }

private:
    struct PoolEntry {
        std::size_t load = 0;
        std::uint32_t value = 0;
        Label label = 0;
        bool is_label = false;
    };

    Program& EmitLoadFromPool(unsigned destination) {
        // ldr rd, [pc, #offset], the offset filled in by Bytes
        constexpr std::uint32_t LoadFromPc = 0x059F0000;
        return Emit(AlwaysCondition | LoadFromPc | (destination << DestinationShift));
    }

    std::vector<std::uint32_t> words_;
    std::vector<std::size_t> labels_;
    std::vector<PoolEntry> pool_;
};

// Starts a thread at a label: priority in r0, entry in r1, argument in r2,
// stack in r3, the default processor in r4.
void StartThread(Program& program,
                 std::uint32_t priority,
                 Program::Label entry,
                 std::uint32_t argument) {
    program.Constant(R0, priority)
        .AddressOf(R1, entry)
        .Constant(R2, argument)
        .Constant(R3, ChildStackTop)
        .Emit(MoveNot(R4, 1))
        .Call(CreateThreadCall);
}

// A kernel with the program's code, a page of data and a main thread's stack,
// running the program from its first word.
class Machine {
public:
    explicit Machine(const Program& program)
        : kernel_(KernelConfig{
              .regions = {.application = RegionSize, .system = RegionSize, .base = RegionSize}}) {
        AddressSpace& space = kernel_.Space();
        const std::vector<std::uint8_t> code = program.Bytes();
        const auto code_pages = (static_cast<std::uint32_t>(code.size()) + Page - 1) / Page * Page;
        EXPECT_EQ(
            space.CommitFixed(CodeAddress, code_pages, Access::ReadExecute, MemoryState::Code),
            result::Success);
        EXPECT_TRUE(space.Memory().Load(CodeAddress, code));
        EXPECT_EQ(space.CommitFixed(DataAddress, Page, Access::ReadWrite, MemoryState::Private),
                  result::Success);
        for (std::uint32_t offset = 0; offset < Page; offset += 4) {
            EXPECT_TRUE(space.Memory().Write32(DataAddress + offset, Unwritten));
        }
        EXPECT_EQ(
            space.CommitFixed(
                layout::HeapEnd - StackSize, StackSize, Access::ReadWrite, MemoryState::Locked),
            result::Success);
        kernel_.Start(MainThreadStart{.entry_point = CodeAddress,
                                      .stack_top = layout::HeapEnd,
                                      .fpscr = RunFast,
                                      .priority = MainPriority});
    }

    KernelStop Run() {
        return kernel_.Run(Budget);
    }

    [[nodiscard]] std::uint32_t Data(std::uint32_t offset) const {
        return kernel_.Space().Memory().Read32(DataAddress + offset).value_or(Unwritten);
    }

    [[nodiscard]] const Thread& MainThread() const {
        return *kernel_.Threads().Threads().front();
    }

    Kernel& Get() {
        return kernel_;
    }

private:
    Kernel kernel_;
};

TEST(Kernel, RunsANewThreadOfHigherPriorityAtOnce) {
    constexpr std::uint32_t Argument = 7;
    Program program;
    const Program::Label child = program.NewLabel();
    StartThread(program, HigherPriority, child, Argument);
    program.Constant(R5, DataAddress).Emit(Load(R6, R5, FirstSlot)).Call(ExitProcessCall);
    program.Bind(child)
        .Constant(R5, DataAddress)
        .Emit(Store(R0, R5, FirstSlot))
        .Call(ExitThreadCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.MainThread().context.registers.at(R6), Argument)
        << "the child ran before CreateThread returned to the main thread";
    EXPECT_EQ(machine.Get().Threads().Threads().size(), 2U);
}

TEST(Kernel, WakesAWaitingThreadWhenAnotherSignalsTheEvent) {
    Program program;
    const Program::Label child = program.NewLabel();
    // main: an event, its handle saved, a lower priority child, then a wait
    // with no timeout. What the wait answers goes in the second slot.
    program.Emit(MoveImmediate(R1, 0))
        .Call(CreateEventCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R1, R5, FirstSlot));
    StartThread(program, LowerPriority, child, 0);
    program.Constant(R5, DataAddress)
        .Emit(Load(R0, R5, FirstSlot))
        .Emit(MoveNot(R2, 0))
        .Emit(MoveNot(R3, 0))
        .Call(WaitSynchronization1Call)
        .Emit(Store(R0, R5, SecondSlot))
        .Call(ExitProcessCall);
    // child: signal the event.
    program.Bind(child)
        .Constant(R5, DataAddress)
        .Emit(Load(R0, R5, FirstSlot))
        .Call(SignalEventCall)
        .Call(ExitThreadCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(SecondSlot), result::Success.raw);
}

TEST(Kernel, AnswersTimeoutAtOnceForAZeroTimeout) {
    Program program;
    program.Emit(MoveImmediate(R1, 0))
        .Call(CreateEventCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R1, R5, FirstSlot))
        .Emit(Load(R0, R5, FirstSlot))
        .Emit(MoveImmediate(R2, 0))
        .Emit(MoveImmediate(R3, 0))
        .Call(WaitSynchronization1Call)
        .Emit(Store(R0, R5, SecondSlot))
        .Call(ExitProcessCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(SecondSlot), result::Timeout.raw);
}

TEST(Kernel, SaysWhenNoThreadCanEverRunAgain) {
    Program program;
    program.Emit(MoveImmediate(R1, 0))
        .Call(CreateEventCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R1, R5, FirstSlot))
        .Emit(Load(R0, R5, FirstSlot))
        .Emit(MoveNot(R2, 0))
        .Emit(MoveNot(R3, 0))
        .Call(WaitSynchronization1Call)
        .Call(ExitProcessCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::Deadlock);
}

TEST(Kernel, SleepsByMovingTheClockWhenNothingElseRuns) {
    constexpr std::uint32_t OneMillisecond = 1'000'000;
    constexpr std::uint32_t TicksPerMillisecond = 268111;
    Program program;
    program.Constant(R0, OneMillisecond)
        .Emit(MoveImmediate(R1, 0))
        .Call(SleepThreadCall)
        .Call(GetSystemTickCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R0, R5, FirstSlot))
        .Call(ExitProcessCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_GE(machine.Data(FirstSlot), TicksPerMillisecond);
    EXPECT_LT(machine.Get().Executed(), static_cast<std::uint64_t>(TicksPerMillisecond))
        << "the clock jumped; the instructions did not run for it";
}

TEST(Kernel, StopsBeforeASystemCallItDoesNotImplement) {
    Program program;
    program.Emit(MoveImmediate(R0, 0));
    const std::size_t call = program.Size();
    program.Call(SendSyncRequest1Call).Call(ExitProcessCall);

    Machine machine(program);
    const KernelStop stop = machine.Run();
    EXPECT_EQ(stop.reason, KernelStopReason::UnimplementedSystemCall);
    EXPECT_EQ(stop.detail, SendSyncRequest1Call);
    EXPECT_EQ(stop.address, Program::AddressOfWord(call));
    EXPECT_EQ(machine.MainThread().context.registers.at(arm::ProgramCounter),
              Program::AddressOfWord(call))
        << "left before the call, to carry on from once it is implemented";
}

TEST(Kernel, CommitsLinearMemoryAtTheBottomOfTheRegion) {
    constexpr std::uint32_t CommitLinear = 0x10003;
    Program program;
    program.Constant(R0, CommitLinear)
        .Emit(MoveImmediate(R1, 0))
        .Emit(MoveImmediate(R2, 0))
        .Constant(R3, Page)
        .Emit(MoveImmediate(R4, 3))
        .Call(ControlMemoryCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R0, R5, FirstSlot))
        .Emit(Store(R1, R5, SecondSlot))
        .Call(ExitProcessCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(FirstSlot), result::Success.raw);
    EXPECT_EQ(machine.Data(SecondSlot), layout::OldLinearHeapBase);
}

TEST(Kernel, RefusesAMisalignedSize) {
    constexpr std::uint32_t CommitHeap = 3;
    constexpr std::uint32_t NotAPage = 0x10;
    Program program;
    program.Constant(R0, CommitHeap)
        .Constant(R1, layout::HeapBase)
        .Emit(MoveImmediate(R2, 0))
        .Constant(R3, NotAPage)
        .Emit(MoveImmediate(R4, 3))
        .Call(ControlMemoryCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R0, R5, FirstSlot))
        .Call(ExitProcessCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(FirstSlot), result::MisalignedSize.raw);
}

TEST(Kernel, LetsOnlyTheHolderReleaseAMutex) {
    Program program;
    const Program::Label child = program.NewLabel();
    // main: a mutex created locked, then a child that tries to release it.
    program.Emit(MoveImmediate(R1, 1))
        .Call(CreateMutexCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R1, R5, FirstSlot));
    StartThread(program, HigherPriority, child, 0);
    program.Constant(R5, DataAddress)
        .Emit(Load(R0, R5, FirstSlot))
        .Call(ReleaseMutexCall)
        .Emit(Store(R0, R5, ThirdSlot))
        .Call(ExitProcessCall);
    program.Bind(child)
        .Constant(R5, DataAddress)
        .Emit(Load(R0, R5, FirstSlot))
        .Call(ReleaseMutexCall)
        .Emit(Store(R0, R5, SecondSlot))
        .Call(ExitThreadCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(SecondSlot), result::WrongLockingThread.raw);
    EXPECT_EQ(machine.Data(ThirdSlot), result::Success.raw);
}

TEST(Kernel, ArbitrationDecrementsWaitsAndWakesOnSignal) {
    constexpr std::uint32_t DecrementAndWaitIfLessThan = 2;
    constexpr std::uint32_t Signal = 0;
    constexpr std::uint32_t Marker = 5;
    Program program;
    const Program::Label child = program.NewLabel();
    // main: an arbiter, a counter of zero, a child that waits on the counter.
    program.Call(CreateAddressArbiterCall)
        .Constant(R5, DataAddress)
        .Emit(Store(R1, R5, FirstSlot))
        .Emit(MoveImmediate(R6, 0))
        .Emit(Store(R6, R5, SecondSlot));
    StartThread(program, HigherPriority, child, 0);
    // The child is waiting: copy the counter, then signal one waiter.
    program.Constant(R5, DataAddress)
        .Emit(Load(R6, R5, SecondSlot))
        .Emit(Store(R6, R5, FourthSlot))
        .Emit(Load(R0, R5, FirstSlot))
        .Constant(R1, DataAddress + SecondSlot)
        .Emit(MoveImmediate(R2, Signal))
        .Emit(MoveImmediate(R3, 1))
        .Call(ArbitrateAddressCall)
        .Call(ExitProcessCall);
    program.Bind(child)
        .Constant(R6, DataAddress)
        .Emit(Load(R0, R6, FirstSlot))
        .Constant(R1, DataAddress + SecondSlot)
        .Emit(MoveImmediate(R2, DecrementAndWaitIfLessThan))
        .Emit(MoveImmediate(R3, 1))
        .Emit(MoveImmediate(R4, 0))
        .Emit(MoveImmediate(R5, 0))
        .Call(ArbitrateAddressCall)
        .Emit(MoveImmediate(R0, Marker))
        .Emit(Store(R0, R6, ThirdSlot))
        .Call(ExitThreadCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(FourthSlot), 0xFFFFFFFFU) << "decremented before it slept";
    EXPECT_EQ(machine.Data(ThirdSlot), Marker) << "woken by the signal";
}

TEST(Kernel, NamesEverySystemCallFrom3dbrew) {
    EXPECT_EQ(Kernel::SystemCallName(0x01), "ControlMemory");
    EXPECT_EQ(Kernel::SystemCallName(0x32), "SendSyncRequest");
    EXPECT_EQ(Kernel::SystemCallName(0x3C), "Break");
    EXPECT_EQ(Kernel::SystemCallName(0x7D), "QueryProcessMemory");
    EXPECT_EQ(Kernel::SystemCallName(0x7F), "Unused");
    EXPECT_EQ(Kernel::SystemCallName(0x1234), "Unknown");
    EXPECT_TRUE(Kernel::IsImplemented(0x24));
    EXPECT_TRUE(Kernel::IsImplemented(0x32));
    EXPECT_FALSE(Kernel::IsImplemented(0x2E)) << "SendSyncRequest1 is not used";
}

// A service that answers command 1 with its argument plus one, and does not
// implement any other.
class Incrementer final : public SessionHandler {
public:
    [[nodiscard]] std::string_view Name() const override {
        return "inc:u";
    }

    bool Handle(IpcRequest& request) override {
        if (request.Command() != 1) {
            return false;
        }
        const std::uint32_t value = request.Pop();
        request.Begin(2, 0);
        request.Push(result::Success);
        request.Push(value + 1);
        return true;
    }
};

// Writes the port name "inc:u" into the data page and connects to it, leaving
// the session handle in r1.
void ConnectToIncrementer(Program& program) {
    constexpr std::uint32_t NameLow = 0x3A636E69;   // "inc:"
    constexpr std::uint32_t NameHigh = 0x00000075;  // "u"
    constexpr std::uint32_t NameOffset = 0x100;
    program.Constant(R5, DataAddress)
        .Constant(R6, NameLow)
        .Emit(Store(R6, R5, NameOffset))
        .Constant(R6, NameHigh)
        .Emit(Store(R6, R5, NameOffset + 4))
        .Constant(R1, DataAddress + NameOffset)
        .Call(ConnectToPortCall);
}

// Puts a request with one normal word into the command buffer, which starts
// 0x80 bytes into the thread's local storage, and sends it on the session in
// r1.
void SendToIncrementer(Program& program, std::uint32_t header, std::uint32_t argument) {
    constexpr std::uint32_t ReadThreadId = 0xEE1D4F70;  // mrc p15, 0, r4, c13, c0, 3
    constexpr std::uint32_t CommandBuffer = 0x80;
    program.Emit(ReadThreadId)
        .Constant(R6, header)
        .Emit(Store(R6, R4, CommandBuffer))
        .Constant(R6, argument)
        .Emit(Store(R6, R4, CommandBuffer + 4))
        .Emit(MoveImmediate(R0, 0))
        .Emit(Store(R1, R5, FirstSlot))
        .Emit(Load(R0, R5, FirstSlot))
        .Call(SendSyncRequestCall)
        .Emit(Load(R6, R4, CommandBuffer + 8))
        .Emit(Store(R6, R5, SecondSlot));
}

TEST(Kernel, CarriesARequestToAServiceAndItsReplyBack) {
    constexpr std::uint32_t Argument = 41;
    Program program;
    ConnectToIncrementer(program);
    SendToIncrementer(program, ipc::MakeHeader(1, 1, 0), Argument);
    program.Call(ExitProcessCall);

    Machine machine(program);
    machine.Get().RegisterPort("inc:u", std::make_shared<Incrementer>());
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(SecondSlot), Argument + 1);
}

TEST(Kernel, ShowsEveryObserverEachRequest) {
    Program program;
    ConnectToIncrementer(program);
    SendToIncrementer(program, ipc::MakeHeader(1, 1, 0), 0);
    program.Call(ExitProcessCall);

    Machine machine(program);
    machine.Get().RegisterPort("inc:u", std::make_shared<Incrementer>());
    std::vector<std::uint32_t> first;
    std::vector<std::string> second;
    machine.Get().OnServiceRequest(
        [&first](const ServiceRequestRecord& record) { first.push_back(record.header); });
    machine.Get().OnServiceRequest(
        [&second](const ServiceRequestRecord& record) { second.emplace_back(record.service); });
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(first, std::vector<std::uint32_t>{ipc::MakeHeader(1, 1, 0)});
    EXPECT_EQ(second, std::vector<std::string>{"inc:u"});
}

TEST(Kernel, StopsWhereItIsAskedToOnceTheCallIsAnswered) {
    Program program;
    ConnectToIncrementer(program);
    SendToIncrementer(program, ipc::MakeHeader(1, 1, 0), 0);
    program.Call(ExitProcessCall);

    Machine machine(program);
    machine.Get().RegisterPort("inc:u", std::make_shared<Incrementer>());
    machine.Get().OnServiceRequest(
        [&machine](const ServiceRequestRecord&) { machine.Get().RequestStop(); });
    const KernelStop stop = machine.Run();
    EXPECT_EQ(stop.reason, KernelStopReason::Requested);
    EXPECT_EQ(machine.MainThread().context.registers.at(arm::ProgramCounter), stop.address)
        << "the thread is after its call, ready to carry on";
}

TEST(Kernel, StopsAtACommandTheServiceDoesNotImplement) {
    constexpr std::uint32_t Unknown = ipc::MakeHeader(2, 1, 0);
    Program program;
    ConnectToIncrementer(program);
    SendToIncrementer(program, Unknown, 0);
    program.Call(ExitProcessCall);

    Machine machine(program);
    machine.Get().RegisterPort("inc:u", std::make_shared<Incrementer>());
    const KernelStop stop = machine.Run();
    EXPECT_EQ(stop.reason, KernelStopReason::UnimplementedServiceCommand);
    EXPECT_EQ(stop.service, "inc:u");
    EXPECT_EQ(stop.detail, Unknown);
    EXPECT_EQ(machine.MainThread().context.registers.at(arm::ProgramCounter), stop.address);
}

TEST(Kernel, RefusesAPortThatDoesNotExist) {
    Program program;
    ConnectToIncrementer(program);
    program.Emit(Store(R0, R5, FirstSlot)).Call(ExitProcessCall);

    Machine machine(program);
    EXPECT_EQ(machine.Run().reason, KernelStopReason::ProcessExited);
    EXPECT_EQ(machine.Data(FirstSlot), result::NotFound.raw);
}

// A service that implements command 1 but not for the request it is sent,
// and says why.
class Decliner final : public SessionHandler {
public:
    [[nodiscard]] std::string_view Name() const override {
        return "inc:u";
    }

    bool Handle(IpcRequest& request) override {
        request.Decline("argument " + std::to_string(request.Pop()));
        return false;
    }
};

TEST(Kernel, StopsWithTheReasonAServiceDeclined) {
    constexpr std::uint32_t Argument = 7;
    Program program;
    ConnectToIncrementer(program);
    SendToIncrementer(program, ipc::MakeHeader(1, 1, 0), Argument);
    program.Call(ExitProcessCall);

    Machine machine(program);
    machine.Get().RegisterPort("inc:u", std::make_shared<Decliner>());
    const KernelStop stop = machine.Run();
    EXPECT_EQ(stop.reason, KernelStopReason::UnimplementedServiceCommand);
    EXPECT_EQ(stop.service, "inc:u (argument 7)");
}

// A kernel with nothing running, for what services ask of it directly.
Kernel IdleKernel() {
    return Kernel(KernelConfig{
        .regions = {.application = RegionSize, .system = RegionSize, .base = RegionSize}});
}

TEST(Kernel, TakesServiceMemoryFromTheSystemOrTheBaseRegion) {
    Kernel kernel = IdleKernel();
    const auto system = kernel.CreateSystemMemoryBlock(Page, 1, Kernel::Region::System);
    const auto base = kernel.CreateSystemMemoryBlock(Page, 1);
    const auto next = kernel.CreateSystemMemoryBlock(Page, 1, Kernel::Region::System);
    EXPECT_EQ(system->linear_address, layout::OldLinearHeapBase + RegionSize);
    EXPECT_EQ(base->linear_address, layout::OldLinearHeapBase + (2 * RegionSize));
    EXPECT_EQ(next->linear_address, system->linear_address + Page);
}

TEST(Kernel, MapsTheDeviceMemoryTheHeaderGrants) {
    constexpr std::uint32_t DspStart = 0x1FF50000;
    constexpr std::uint32_t DspEnd = 0x1FF58000;
    constexpr std::uint32_t VramEnd = 0x1F001000;
    constexpr std::uint32_t Registers = 0x1EC00000;
    constexpr std::uint32_t Marker = 0x5A5A5A5A;
    Kernel kernel = IdleKernel();
    const std::vector<ctr::MappedRange> ranges = {
        {.start = DspStart, .end = DspEnd, .read_only = false, .is_static = true},
        {.start = Kernel::VramBase, .end = VramEnd, .read_only = true, .is_static = false},
        {.start = Registers, .end = Registers + Page, .read_only = false, .is_static = false},
    };
    kernel.MapDeviceMemory(ranges);

    memory::ProcessMemory& memory = kernel.Space().Memory();
    EXPECT_TRUE(memory.Write32(DspStart, Marker));
    EXPECT_EQ(kernel.DspMemory()->Bytes()[DspStart - Kernel::DspMemoryBase], 0x5A)
        << "the process sees the DSP's own memory";
    EXPECT_FALSE(memory.Write32(Kernel::VramBase, Marker)) << "granted read-only";
    EXPECT_TRUE(memory.Read32(Kernel::VramBase).has_value());
    EXPECT_FALSE(memory.Read32(DspEnd).has_value()) << "only what was granted";
    EXPECT_FALSE(memory.Read32(Registers).has_value()) << "registers are reached through services";
}

}  // namespace
}  // namespace hexlith::domain::kernel
