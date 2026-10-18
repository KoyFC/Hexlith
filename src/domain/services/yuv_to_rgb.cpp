#include "domain/services/yuv_to_rgb.h"

#include "domain/video/yuv_conversion.h"

#include <algorithm>
#include <utility>

namespace hexlith::domain::services {
namespace {

// The four standard sets of coefficients, as signed halfwords. Source:
// standard_coefficients in Azahar's y2r_u.cpp: ITU Rec. 601 and 709, each
// with and without scaling.
constexpr std::size_t StandardSets = 4;
constexpr std::array<std::array<std::int16_t, 8>, StandardSets> StandardCoefficients = {{
    {{0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}},
    {{0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51}},
    {{0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B}},
    {{0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421}},
}};

// The limits ConversionConfiguration puts on a conversion's size. Source:
// SetInputLineWidth and SetInputLines in y2r_u.cpp.
constexpr std::uint32_t MaximumLength = 1024;
constexpr std::uint32_t LineWidthMultiple = 8;

// The output's buffer, after Y, U, V and YUYV.
constexpr std::size_t OutputTransfer = 4;

// A bool travels in the low byte of its word.
constexpr std::uint32_t ByteMask = 0xFF;
constexpr std::uint32_t HalfwordMask = 0xFFFF;

std::uint32_t PackHalfwords(std::int16_t low, std::int16_t high) {
    return static_cast<std::uint16_t>(low) |
           (static_cast<std::uint32_t>(static_cast<std::uint16_t>(high)) << 16U);
}

}  // namespace

YuvToRgb::YuvToRgb(kernel::Kernel& kernel) : kernel_(&kernel) {}

std::span<const YuvToRgb::Command> YuvToRgb::Commands() {
    using Y = YuvToRgb;
    static constexpr std::array<Command, 45> Table = {{
        {.id = 0x0001, .name = "SetInputFormat", .handler = &Y::Set<Setting::InputFormat>},
        {.id = 0x0002, .name = "GetInputFormat", .handler = &Y::Get<Setting::InputFormat>},
        {.id = 0x0003, .name = "SetOutputFormat", .handler = &Y::Set<Setting::OutputFormat>},
        {.id = 0x0004, .name = "GetOutputFormat", .handler = &Y::Get<Setting::OutputFormat>},
        {.id = 0x0005, .name = "SetRotation", .handler = &Y::Set<Setting::Rotation>},
        {.id = 0x0006, .name = "GetRotation", .handler = &Y::Get<Setting::Rotation>},
        {.id = 0x0007, .name = "SetBlockAlignment", .handler = &Y::Set<Setting::BlockAlignment>},
        {.id = 0x0008, .name = "GetBlockAlignment", .handler = &Y::Get<Setting::BlockAlignment>},
        {.id = 0x0009,
         .name = "SetSpacialDithering",
         .handler = &Y::Set<Setting::SpacialDithering>},
        {.id = 0x000A,
         .name = "GetSpacialDithering",
         .handler = &Y::Get<Setting::SpacialDithering>},
        {.id = 0x000B,
         .name = "SetTemporalDithering",
         .handler = &Y::Set<Setting::TemporalDithering>},
        {.id = 0x000C,
         .name = "GetTemporalDithering",
         .handler = &Y::Get<Setting::TemporalDithering>},
        {.id = 0x000D,
         .name = "SetTransferEndInterrupt",
         .handler = &Y::Set<Setting::TransferEndInterrupt>},
        {.id = 0x000E,
         .name = "GetTransferEndInterrupt",
         .handler = &Y::Get<Setting::TransferEndInterrupt>},
        {.id = 0x000F, .name = "GetTransferEndEvent", .handler = &Y::GetTransferEndEvent},
        {.id = 0x0010, .name = "SetSendingY", .handler = &Y::SetTransfer<0>},
        {.id = 0x0011, .name = "SetSendingU", .handler = &Y::SetTransfer<1>},
        {.id = 0x0012, .name = "SetSendingV", .handler = &Y::SetTransfer<2>},
        {.id = 0x0013, .name = "SetSendingYUYV", .handler = &Y::SetTransfer<3>},
        {.id = 0x0014, .name = "IsFinishedSendingYuv", .handler = &Y::AnswerFinished},
        {.id = 0x0015, .name = "IsFinishedSendingY", .handler = &Y::AnswerFinished},
        {.id = 0x0016, .name = "IsFinishedSendingU", .handler = &Y::AnswerFinished},
        {.id = 0x0017, .name = "IsFinishedSendingV", .handler = &Y::AnswerFinished},
        {.id = 0x0018, .name = "SetReceiving", .handler = &Y::SetTransfer<4>},
        {.id = 0x0019, .name = "IsFinishedReceiving", .handler = &Y::AnswerFinished},
        {.id = 0x001A, .name = "SetInputLineWidth", .handler = &Y::SetInputLineWidth},
        {.id = 0x001B, .name = "GetInputLineWidth", .handler = &Y::GetInputLineWidth},
        {.id = 0x001C, .name = "SetInputLines", .handler = &Y::SetInputLines},
        {.id = 0x001D, .name = "GetInputLines", .handler = &Y::GetInputLines},
        {.id = 0x001E, .name = "SetCoefficient", .handler = &Y::SetCoefficient},
        {.id = 0x001F, .name = "GetCoefficient", .handler = &Y::GetCoefficient},
        {.id = 0x0020, .name = "SetStandardCoefficient", .handler = &Y::SetStandardCoefficient},
        {.id = 0x0021, .name = "GetStandardCoefficient", .handler = &Y::GetStandardCoefficient},
        {.id = 0x0022, .name = "SetAlpha", .handler = &Y::Set<Setting::Alpha>},
        {.id = 0x0023, .name = "GetAlpha", .handler = &Y::Get<Setting::Alpha>},
        {.id = 0x0024, .name = "SetDitheringWeightParams", .handler = &Y::SetDitheringWeights},
        {.id = 0x0025, .name = "GetDitheringWeightParams", .handler = &Y::GetDitheringWeights},
        {.id = 0x0026, .name = "StartConversion", .handler = &Y::StartConversion},
        {.id = 0x0027, .name = "StopConversion", .handler = &Y::StopConversion},
        {.id = 0x0028, .name = "IsBusyConversion", .handler = &Y::IsBusyConversion},
        {.id = 0x0029, .name = "SetPackageParameter", .handler = &Y::SetPackageParameter},
        {.id = 0x002A, .name = "PingProcess", .handler = &Y::PingProcess},
        {.id = 0x002B, .name = "DriverInitialize", .handler = &Y::DriverInitialize},
        {.id = 0x002C, .name = "DriverFinalize", .handler = &Y::DriverFinalize},
        {.id = 0x002D, .name = "GetPackageParameter"},
    }};
    return Table;
}

template<YuvToRgb::Setting Which>
void YuvToRgb::Set(kernel::IpcRequest& request) {
    // The enums and the bools are a byte, the alpha a halfword.
    const std::uint32_t mask = Which == Setting::Alpha ? HalfwordMask : ByteMask;
    settings_.at(static_cast<std::size_t>(Which)) = request.Pop() & mask;
    request.ReplyWith(kernel::result::Success);
}

template<YuvToRgb::Setting Which>
void YuvToRgb::Get(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(settings_.at(static_cast<std::size_t>(Which)));
}

void YuvToRgb::GetTransferEndEvent(kernel::IpcRequest& request) {
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    const std::array<std::shared_ptr<kernel::Object>, 1> objects = {completion_};
    request.PushCopyObjects(objects);
}

template<std::size_t Index>
void YuvToRgb::SetTransfer(kernel::IpcRequest& request) {
    Transfer& transfer = transfers_.at(Index);
    transfer.address = request.Pop();
    transfer.size = request.Pop();
    transfer.unit = request.Pop() & HalfwordMask;
    transfer.gap = request.Pop() & HalfwordMask;
    static_cast<void>(request.PopHandles());  // the process that owns the buffer
    request.ReplyWith(kernel::result::Success);
}

// Every transfer has finished as soon as it is asked about, as in Azahar,
// which moves the data at the start of the conversion.
void YuvToRgb::AnswerFinished(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(1);
}

kernel::ResultCode YuvToRgb::ChangeLineWidth(std::uint32_t width) {
    if (width == 0 || width > MaximumLength || width % LineWidthMultiple != 0) {
        return OutOfRange;
    }
    line_width_ = width;
    return kernel::result::Success;
}

// The console's driver does not write a count of 1024 to the hardware at
// all, so the last count set stays, which Azahar copies.
kernel::ResultCode YuvToRgb::ChangeLines(std::uint32_t lines) {
    if (lines == 0 || lines > MaximumLength) {
        return OutOfRange;
    }
    if (lines != MaximumLength) {
        lines_ = lines;
    }
    return kernel::result::Success;
}

kernel::ResultCode YuvToRgb::ChangeStandardCoefficient(std::uint32_t index) {
    if (index >= StandardSets) {
        return InvalidEnumValue;
    }
    const std::array<std::int16_t, CoefficientCount>& set = StandardCoefficients.at(index);
    for (std::size_t word = 0; word < coefficients_.size(); ++word) {
        coefficients_.at(word) = PackHalfwords(set.at(word * 2), set.at((word * 2) + 1));
    }
    return kernel::result::Success;
}

void YuvToRgb::SetInputLineWidth(kernel::IpcRequest& request) {
    request.ReplyWith(ChangeLineWidth(request.Pop()));
}

void YuvToRgb::GetInputLineWidth(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(line_width_);
}

void YuvToRgb::SetInputLines(kernel::IpcRequest& request) {
    request.ReplyWith(ChangeLines(request.Pop()));
}

void YuvToRgb::GetInputLines(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(lines_);
}

void YuvToRgb::SetCoefficient(kernel::IpcRequest& request) {
    for (std::uint32_t& word : coefficients_) {
        word = request.Pop();
    }
    request.ReplyWith(kernel::result::Success);
}

void YuvToRgb::GetCoefficient(kernel::IpcRequest& request) {
    request.Begin(1 + static_cast<std::uint32_t>(coefficients_.size()), 0);
    request.Push(kernel::result::Success);
    for (const std::uint32_t word : coefficients_) {
        request.Push(word);
    }
}

void YuvToRgb::SetStandardCoefficient(kernel::IpcRequest& request) {
    request.ReplyWith(ChangeStandardCoefficient(request.Pop()));
}

void YuvToRgb::GetStandardCoefficient(kernel::IpcRequest& request) {
    const std::uint32_t index = request.Pop();
    if (index >= StandardSets) {
        request.ReplyWith(InvalidEnumValue);
        return;
    }
    const std::array<std::int16_t, CoefficientCount>& set = StandardCoefficients.at(index);
    request.Begin(1 + (CoefficientCount / 2), 0);
    request.Push(kernel::result::Success);
    for (std::size_t word = 0; word < CoefficientCount / 2; ++word) {
        request.Push(PackHalfwords(set.at(word * 2), set.at((word * 2) + 1)));
    }
}

void YuvToRgb::SetDitheringWeights(kernel::IpcRequest& request) {
    for (std::uint32_t& word : dithering_weights_) {
        word = request.Pop();
    }
    request.ReplyWith(kernel::result::Success);
}

void YuvToRgb::GetDitheringWeights(kernel::IpcRequest& request) {
    request.Begin(1 + static_cast<std::uint32_t>(dithering_weights_.size()), 0);
    request.Push(kernel::result::Success);
    for (const std::uint32_t word : dithering_weights_) {
        request.Push(word);
    }
}

std::vector<std::uint8_t> YuvToRgb::Gather(const Transfer& transfer, std::size_t bytes) const {
    std::vector<std::uint8_t> gathered;
    if (transfer.unit == 0) {
        return gathered;
    }
    gathered.reserve(bytes);
    std::uint32_t address = transfer.address;
    std::vector<std::uint8_t> unit(transfer.unit);
    while (gathered.size() < bytes) {
        if (!kernel_->Space().Memory().Copy(address, unit).Ok()) {
            return {};
        }
        const std::size_t taken = std::min<std::size_t>(unit.size(), bytes - gathered.size());
        gathered.insert(
            gathered.end(), unit.begin(), unit.begin() + static_cast<std::ptrdiff_t>(taken));
        address += transfer.unit + transfer.gap;
    }
    return gathered;
}

void YuvToRgb::Scatter(const Transfer& transfer, std::span<const std::uint8_t> bytes) {
    if (transfer.unit == 0) {
        return;
    }
    std::uint32_t address = transfer.address;
    for (std::size_t at = 0; at < bytes.size(); at += transfer.unit) {
        const std::size_t length = std::min<std::size_t>(transfer.unit, bytes.size() - at);
        if (!kernel_->Space().Memory().Load(address, bytes.subspan(at, length)).Ok()) {
            return;
        }
        address += transfer.unit + transfer.gap;
    }
}

void YuvToRgb::Convert() {
    const auto setting = [this](Setting which) {
        return settings_.at(static_cast<std::size_t>(which));
    };
    video::YuvConversion conversion{
        .input = static_cast<video::YuvFormat>(setting(Setting::InputFormat)),
        .output = static_cast<video::RgbFormat>(setting(Setting::OutputFormat)),
        .rotation = static_cast<video::Rotation>(setting(Setting::Rotation)),
        .layout = static_cast<video::OutputLayout>(setting(Setting::BlockAlignment)),
        .width = line_width_,
        .lines = lines_,
        .coefficients = {},
        .alpha = static_cast<std::uint8_t>(setting(Setting::Alpha))};
    for (std::size_t index = 0; index < conversion.coefficients.size(); ++index) {
        const std::uint32_t word = coefficients_.at(index / 2);
        conversion.coefficients.at(index) =
            static_cast<std::int16_t>(index % 2 == 0 ? word & HalfwordMask : word >> 16U);
    }
    if (!video::IsConvertible(conversion)) {
        return;
    }

    // A wide format's samples are the low bytes of halfwords.
    const std::size_t stride = video::BytesPerSample(conversion.input);
    const auto plane = [&](video::YuvPlane which, std::size_t transfer) {
        std::vector<std::uint8_t> bytes =
            Gather(transfers_.at(transfer), video::SamplesNeeded(conversion, which) * stride);
        std::vector<std::uint8_t> samples;
        samples.reserve(bytes.size() / stride);
        for (std::size_t at = 0; at < bytes.size(); at += stride) {
            samples.push_back(bytes[at]);
        }
        return samples;
    };
    const std::vector<std::uint8_t> y = plane(video::YuvPlane::Y, 0);
    const std::vector<std::uint8_t> u = plane(video::YuvPlane::U, 1);
    const std::vector<std::uint8_t> v = plane(video::YuvPlane::V, 2);
    const std::vector<std::uint8_t> interleaved = plane(video::YuvPlane::Interleaved, 3);
    for (const auto& [samples, which] : {std::pair{&y, video::YuvPlane::Y},
                                         std::pair{&u, video::YuvPlane::U},
                                         std::pair{&v, video::YuvPlane::V},
                                         std::pair{&interleaved, video::YuvPlane::Interleaved}}) {
        if (samples->size() < video::SamplesNeeded(conversion, which)) {
            return;
        }
    }
    const std::vector<std::uint8_t> output = video::ConvertYuvToRgb(
        conversion, video::YuvPlanes{.y = y, .u = u, .v = v, .interleaved = interleaved});
    Scatter(transfers_.at(OutputTransfer), output);
}

void YuvToRgb::StartConversion(kernel::IpcRequest& request) {
    Convert();
    ++conversion_;
    busy_ = true;
    const std::uint64_t started = conversion_;
    kernel::Scheduler& scheduler = kernel_->Threads();
    scheduler.ScheduleAt(scheduler.Ticks() + ConversionTicks, [this, started] {
        if (!busy_ || started != conversion_) {
            return;
        }
        busy_ = false;
        kernel_->Signal(*completion_);
    });
    request.ReplyWith(kernel::result::Success);
}

void YuvToRgb::StopConversion(kernel::IpcRequest& request) {
    busy_ = false;
    request.ReplyWith(kernel::result::Success);
}

void YuvToRgb::IsBusyConversion(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(busy_ ? 1 : 0);
}

// Three words: the four format bytes, then the line width and the line count
// as halfwords, then the coefficient set, a padding byte and the alpha.
// Source: ConversionParameters in Azahar's y2r_u.h.
void YuvToRgb::SetPackageParameter(kernel::IpcRequest& request) {
    constexpr unsigned SecondByte = 8;
    constexpr unsigned ThirdByte = 16;
    constexpr unsigned FourthByte = 24;
    constexpr unsigned HighHalfword = 16;
    const std::uint32_t formats = request.Pop();
    const std::uint32_t sizes = request.Pop();
    const std::uint32_t rest = request.Pop();

    settings_.at(static_cast<std::size_t>(Setting::InputFormat)) = formats & ByteMask;
    settings_.at(static_cast<std::size_t>(Setting::OutputFormat)) =
        (formats >> SecondByte) & ByteMask;
    settings_.at(static_cast<std::size_t>(Setting::Rotation)) = (formats >> ThirdByte) & ByteMask;
    settings_.at(static_cast<std::size_t>(Setting::BlockAlignment)) =
        (formats >> FourthByte) & ByteMask;

    // Each step stops at the first that fails, with what was set so far kept,
    // as Azahar's does.
    kernel::ResultCode result = ChangeLineWidth(sizes & HalfwordMask);
    if (!result.IsError()) {
        result = ChangeLines(sizes >> HighHalfword);
    }
    if (!result.IsError()) {
        result = ChangeStandardCoefficient(rest & ByteMask);
    }
    if (!result.IsError()) {
        settings_.at(static_cast<std::size_t>(Setting::Alpha)) = rest >> HighHalfword;
    }
    request.ReplyWith(result);
}

void YuvToRgb::PingProcess(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(0);
}

// Back to the state the driver starts in: the formats, the rotation and the
// alignment at their first values, a conversion of 1024 by 1024, no
// coefficients, no buffers. Source: DriverInitialize in y2r_u.cpp.
void YuvToRgb::DriverInitialize(kernel::IpcRequest& request) {
    for (const Setting setting : {Setting::InputFormat,
                                  Setting::OutputFormat,
                                  Setting::Rotation,
                                  Setting::BlockAlignment,
                                  Setting::Alpha}) {
        settings_.at(static_cast<std::size_t>(setting)) = 0;
    }
    coefficients_.fill(0);
    static_cast<void>(ChangeLineWidth(MaximumLength));
    static_cast<void>(ChangeLines(MaximumLength));
    transfers_.fill(Transfer{});
    completion_->signaled = false;
    request.ReplyWith(kernel::result::Success);
}

void YuvToRgb::DriverFinalize(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

}  // namespace hexlith::domain::services
