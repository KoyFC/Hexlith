#ifndef HEXLITH_DOMAIN_SERVICES_GRAPHICS_H
#define HEXLITH_DOMAIN_SERVICES_GRAPHICS_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <array>
#include <cstdint>
#include <functional>
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
// Nothing is drawn. A command in the queue raises the interrupt the GPU
// raises when it finishes it, once the time Azahar gives it has passed on the
// virtual clock: the copy at the rates it measured on hardware, a list at its
// cost per command, and 300 microseconds of synchronisation on each. A memory
// fill writes its values and raises PSC0 or PSC1, a texture copy copies and
// raises PPF, a DMA copies and raises DMA, a display transfer raises PPF
// without converting anything, and a command list raises P3D without being
// run. Both screens' vertical blanks, PDC0 and PDC1, arrive every 4,481,136
// ticks, the frame time Azahar gives as measured on hardware. At each, the
// framebuffer the game asked that screen to show next in shared memory
// becomes the one it shows; SetBufferSwap does the same at once.
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

    // One command taken from the queue, as the game wrote it, with the frame
    // it was taken in.
    struct GxCommand {
        std::uint64_t frame = 0;
        std::array<std::uint32_t, 8> words{};
    };

    // Called with each command before it is carried out.
    void OnCommand(std::function<void(const GxCommand&)> observer) {
        command_observer_ = std::move(observer);
    }

    // What a screen is told to show, as the game describes it: which of its
    // two framebuffers the addresses are for, the left and right eye's
    // address, the bytes from one row to the next, the pixel format, and
    // which of the two to show. Source: FrameBufferInfo in Azahar's
    // gsp_gpu.h; 3dbrew, GSP Shared Memory.
    struct FramebufferInfo {
        std::uint32_t active = 0;
        std::uint32_t address_left = 0;
        std::uint32_t address_right = 0;
        std::uint32_t stride = 0;
        std::uint32_t format = 0;
        std::uint32_t shown = 0;
        std::uint32_t unknown = 0;
    };

    // The top screen is 0 and the bottom 1.
    struct FramebufferSwap {
        std::uint64_t frame = 0;
        std::uint32_t screen = 0;
        FramebufferInfo info;
    };

    // Called at the top screen's vertical blank with the frame it begins,
    // before anything is raised.
    void OnVerticalBlank(std::function<void(std::uint64_t)> observer) {
        vertical_blank_observer_ = std::move(observer);
    }

    // Called each time a screen is given a framebuffer to show.
    void OnFramebuffer(std::function<void(const FramebufferSwap&)> observer) {
        framebuffer_observer_ = std::move(observer);
    }

    // Command IDs. Source: CommandId in Azahar's gsp_command.h.
    static constexpr std::uint32_t SubmitCommandListId = 1;

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
    // Shows what each client asked the screen to show next, if it asked.
    void TakeFramebufferUpdates(std::uint32_t screen);
    void Swap(std::uint32_t screen, const FramebufferInfo& info);
    void Raise(Interrupt interrupt);
    // Raises an interrupt once a GPU operation that takes this long is done,
    // with the time every request costs on top.
    void RaiseAfter(Interrupt interrupt, std::uint64_t nanoseconds);
    void Dma(std::uint32_t offset);
    void CommandList(std::uint32_t offset);
    void Fill(std::uint32_t offset);
    void
    FillRange(std::uint32_t start, std::uint32_t end, std::uint32_t value, std::uint32_t control);
    void Transfer(std::uint32_t offset);
    void Copy(std::uint32_t offset);
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
    // When the last operation started will be done.
    std::uint64_t busy_until_ = 0;
    std::function<void(const GxCommand&)> command_observer_;
    std::function<void(const FramebufferSwap&)> framebuffer_observer_;
    std::function<void(std::uint64_t)> vertical_blank_observer_;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_GRAPHICS_H
