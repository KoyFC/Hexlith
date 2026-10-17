#ifndef HEXLITH_DOMAIN_SERVICES_DIGITAL_SIGNAL_PROCESSOR_H
#define HEXLITH_DOMAIN_SERVICES_DIGITAL_SIGNAL_PROCESSOR_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// dsp::DSP, the audio processor. The game loads its firmware, registers
// events for the DSP's interrupts, and talks to it through pipes and through
// two regions of DSP memory it maps. Commands: 3dbrew, DSP Services; answers:
// DSP_DSP in Azahar, src/core/hle/service/dsp/dsp_dsp.cpp, and the DSP's side,
// src/audio_core/hle/hle.cpp.
//
// The firmware is accepted and not run. The audio pipe answers the start-up
// command with the addresses of the structures in DSP memory, as the firmware
// does, and while the DSP is on an audio frame passes every 1,310,720 ticks,
// 160 samples at the DSP's rate, each raising the audio pipe's interrupt. No
// sound is mixed: the structures stay as the game writes them.
class DigitalSignalProcessor final : public CommandTable<DigitalSignalProcessor> {
public:
    explicit DigitalSignalProcessor(kernel::Kernel& kernel);

    [[nodiscard]] std::string_view Name() const override {
        return "dsp::DSP";
    }

    static std::span<const Command> Commands();

    // 160 samples a frame, each 4096 * 2 ticks. Source: audio_frame_ticks in
    // Azahar's hle.cpp; 3dbrew, DSP Memory Region, gives the 160.
    static constexpr std::uint64_t FrameTicks = 160ULL * 4096 * 2;

    static constexpr std::size_t PipeCount = 8;
    static constexpr std::uint32_t AudioPipe = 2;

    [[nodiscard]] bool IsOn() const {
        return on_;
    }

    [[nodiscard]] std::size_t Readable(std::uint32_t pipe) const {
        return pipes_.at(pipe).size();
    }

private:
    // The DSP's three interrupt types, each with an event per pipe.
    static constexpr std::size_t InterruptTypes = 3;
    // The type of interrupt raised when a pipe has a message waiting.
    static constexpr std::uint32_t MessageInterrupt = 2;

    void RecvData(kernel::IpcRequest& request);
    void RecvDataIsReady(kernel::IpcRequest& request);
    void SetSemaphore(kernel::IpcRequest& request);
    void ConvertProcessAddressFromDspDram(kernel::IpcRequest& request);
    void WriteProcessPipe(kernel::IpcRequest& request);
    void ReadPipeIfPossible(kernel::IpcRequest& request);
    void GetPipeReadableSize(kernel::IpcRequest& request);
    void LoadComponent(kernel::IpcRequest& request);
    void FlushDataCache(kernel::IpcRequest& request);
    void RegisterInterruptEvents(kernel::IpcRequest& request);
    void GetSemaphoreEventHandle(kernel::IpcRequest& request);
    void SetSemaphoreMask(kernel::IpcRequest& request);
    void GetHeadphoneStatus(kernel::IpcRequest& request);

    void StartAudio();
    void ScheduleFrame();
    void Interrupt(std::uint32_t type, std::uint32_t pipe);

    // Never null; the kernel outlives its services.
    kernel::Kernel* kernel_;
    std::array<std::vector<std::uint8_t>, PipeCount> pipes_;
    std::array<std::array<std::shared_ptr<kernel::Event>, PipeCount>, InterruptTypes> events_;
    std::shared_ptr<kernel::Event> semaphore_event_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::uint16_t semaphore_ = 0;
    std::uint16_t semaphore_mask_ = 0;
    bool component_loaded_ = false;
    bool on_ = false;
    bool ticking_ = false;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_DIGITAL_SIGNAL_PROCESSOR_H
