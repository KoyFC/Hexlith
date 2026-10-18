#ifndef HEXLITH_DOMAIN_SERVICES_YUV_TO_RGB_H
#define HEXLITH_DOMAIN_SERVICES_YUV_TO_RGB_H

#include "domain/kernel/kernel.h"
#include "domain/services/command_table.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hexlith::domain::services {

// y2r:u, the hardware that turns video frames from YUV into RGB. A title sets
// up a conversion, names the buffers it reads and writes, starts it and waits
// on an event for the end of the transfer. Commands: 3dbrew, Y2R Services;
// answers: Y2R_U in Azahar, src/core/hle/service/cam/y2r_u.cpp.
//
// Every setting is kept and answered back, the checks Azahar makes on the
// line width, the line count and the coefficient set are made, and a
// conversion ends 50,000 ticks after it starts, Azahar's MinY2RDelay, with
// the event signalled. The conversion itself is done at the start, as
// Azahar does it: the samples are gathered from the buffers the title named,
// each in units with a gap after each, converted by video::ConvertYuvToRgb,
// and scattered to the output buffer the same way.
class YuvToRgb final : public CommandTable<YuvToRgb> {
public:
    explicit YuvToRgb(kernel::Kernel& kernel);

    [[nodiscard]] std::string_view Name() const override {
        return "y2r:u";
    }

    static std::span<const Command> Commands();

    // Ticks from StartConversion to the end of the transfer. Source:
    // MinY2RDelay in Azahar's y2r_u.cpp.
    static constexpr std::uint64_t ConversionTicks = 50000;

    // The settings that are one word each, in the order of their commands.
    enum class Setting : std::uint8_t {
        InputFormat,
        OutputFormat,
        Rotation,
        BlockAlignment,
        SpacialDithering,
        TemporalDithering,
        TransferEndInterrupt,
        Alpha,
        Count
    };

    // Results. Source: y2r_u.cpp, which gives each value.
    static constexpr kernel::ResultCode OutOfRange{0xE0E053FD};
    static constexpr kernel::ResultCode InvalidEnumValue{0xE0E053ED};

    [[nodiscard]] bool Busy() const {
        return busy_;
    }

private:
    // A buffer the hardware reads from or writes to.
    struct Transfer {
        std::uint32_t address = 0;
        std::uint32_t size = 0;
        std::uint32_t unit = 0;
        std::uint32_t gap = 0;
    };

    // Halfwords of the coefficients, and of the dithering weights.
    static constexpr std::size_t CoefficientCount = 8;
    static constexpr std::size_t DitheringWeightWords = 8;

    template<Setting Which>
    void Set(kernel::IpcRequest& request);
    template<Setting Which>
    void Get(kernel::IpcRequest& request);

    void GetTransferEndEvent(kernel::IpcRequest& request);
    template<std::size_t Index>
    void SetTransfer(kernel::IpcRequest& request);
    void AnswerFinished(kernel::IpcRequest& request);
    void SetInputLineWidth(kernel::IpcRequest& request);
    void GetInputLineWidth(kernel::IpcRequest& request);
    void SetInputLines(kernel::IpcRequest& request);
    void GetInputLines(kernel::IpcRequest& request);
    void SetCoefficient(kernel::IpcRequest& request);
    void GetCoefficient(kernel::IpcRequest& request);
    void SetStandardCoefficient(kernel::IpcRequest& request);
    void GetStandardCoefficient(kernel::IpcRequest& request);
    void SetDitheringWeights(kernel::IpcRequest& request);
    void GetDitheringWeights(kernel::IpcRequest& request);
    void StartConversion(kernel::IpcRequest& request);
    // Converts the image the settings and buffers describe, if the hardware
    // could. A buffer that cannot be read or written leaves the output as it
    // was, or written in part.
    void Convert();
    [[nodiscard]] std::vector<std::uint8_t> Gather(const Transfer& transfer,
                                                   std::size_t bytes) const;
    void Scatter(const Transfer& transfer, std::span<const std::uint8_t> bytes);
    void StopConversion(kernel::IpcRequest& request);
    void IsBusyConversion(kernel::IpcRequest& request);
    void SetPackageParameter(kernel::IpcRequest& request);
    void PingProcess(kernel::IpcRequest& request);
    void DriverInitialize(kernel::IpcRequest& request);
    void DriverFinalize(kernel::IpcRequest& request);

    [[nodiscard]] kernel::ResultCode ChangeLineWidth(std::uint32_t width);
    [[nodiscard]] kernel::ResultCode ChangeLines(std::uint32_t lines);
    [[nodiscard]] kernel::ResultCode ChangeStandardCoefficient(std::uint32_t index);

    // Never null; the kernel outlives its services.
    kernel::Kernel* kernel_;
    std::shared_ptr<kernel::Event> completion_ =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
    std::array<std::uint32_t, static_cast<std::size_t>(Setting::Count)> settings_{};
    // Y, U, V, YUYV and the output.
    static constexpr std::size_t TransferCount = 5;
    std::array<Transfer, TransferCount> transfers_{};
    std::uint32_t line_width_ = 0;
    std::uint32_t lines_ = 0;
    std::array<std::uint32_t, CoefficientCount / 2> coefficients_{};
    std::array<std::uint32_t, DitheringWeightWords> dithering_weights_{};
    bool busy_ = false;
    // Which conversion the scheduled completion belongs to, so that one
    // stopped or restarted does not signal for the next.
    std::uint64_t conversion_ = 0;
};

}  // namespace hexlith::domain::services

#endif  // HEXLITH_DOMAIN_SERVICES_YUV_TO_RGB_H
