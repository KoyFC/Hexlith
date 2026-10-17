#ifndef HEXLITH_DOMAIN_SERVICES_GRAPHICS_H
#define HEXLITH_DOMAIN_SERVICES_GRAPHICS_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// gsp::Gpu, the GPU as an application drives it: the right to use it, its
// registers, a queue of commands in shared memory, and interrupts delivered
// through a queue in the same memory with an event to say one arrived.
// Commands: 3dbrew, GSP Services; answers: GSP_GPU in Azahar,
// src/core/hle/service/gsp/gsp_gpu.cpp, and the GPU's side of the commands,
// src/video_core/gpu.cpp.
//
// Nothing is drawn. A command in the queue completes at once and raises the
// interrupt the GPU raises when it finishes one: a memory fill PSC0 or PSC1, a
// transfer or a texture copy PPF, a command list P3D, a DMA DMA, which copies
// the memory as asked. Both screens' vertical blanks, PDC0 and PDC1, arrive
// every 4,481,136 ticks, the frame time Azahar gives as measured on hardware.
// Registers keep what is written to them and answer it back.
class Graphics final : public CommandTable<Graphics> {
public:
    explicit Graphics(kernel::Kernel& kernel);

    [[nodiscard]] std::string_view Name() const override {
        return "gsp::Gpu";
    }

    static std::span<const Command> Commands();

    static constexpr std::uint64_t FrameTicks = 4481136;

    // Interrupt IDs. Source: InterruptId in Azahar's gsp_interrupt.h.
    enum class Interrupt : std::uint8_t {
        Psc0 = 0,
        Psc1 = 1,
        Pdc0 = 2,
        Pdc1 = 3,
        Ppf = 4,
        P3d = 5,
        Dma = 6
    };

    // Answered to the first client to register for interrupts, which tells it
    // to set the GPU up. A success code: bit 31 is clear.
    static constexpr kernel::ResultCode FirstInitialization{0x00002A07};

    // Vertical blanks of the top screen since the process started, which is
    // what a frame is counted in.
    [[nodiscard]] std::uint64_t Frames() const {
        return frames_;
    }

    [[nodiscard]] const std::shared_ptr<kernel::SharedMemory>& SharedBlock() const {
        return shared_;
    }

private:
    struct Client {
        std::uint32_t thread_id = 0;
        std::shared_ptr<kernel::Event> event;
    };

    void WriteHwRegs(kernel::IpcRequest& request);
    void WriteHwRegsWithMask(kernel::IpcRequest& request);
    void ReadHwRegs(kernel::IpcRequest& request);
    void Succeed(kernel::IpcRequest& request);
    void SetBufferSwap(kernel::IpcRequest& request);
    void FlushDataCache(kernel::IpcRequest& request);
    void TriggerCmdReqQueue(kernel::IpcRequest& request);
    void RegisterInterruptRelayQueue(kernel::IpcRequest& request);
    void AcquireRight(kernel::IpcRequest& request);

    void ScheduleVerticalBlank();
    void Raise(Interrupt interrupt);
    void Queue(const Client& client, Interrupt interrupt);
    void Execute(std::uint32_t offset);
    [[nodiscard]] std::uint32_t Word(std::uint32_t offset) const;
    void SetWord(std::uint32_t offset, std::uint32_t value);

    // Never null; the kernel outlives its services.
    kernel::Kernel* kernel_;
    std::shared_ptr<kernel::SharedMemory> shared_;
    std::vector<Client> clients_;
    std::uint32_t next_thread_id_ = 0;
    bool has_right_ = false;
    std::map<std::uint32_t, std::uint32_t> registers_;
    std::uint64_t frames_ = 0;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_GRAPHICS_H
