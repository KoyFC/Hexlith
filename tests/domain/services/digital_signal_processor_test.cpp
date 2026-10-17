#include "core/byte_reader.h"
#include "domain/services/digital_signal_processor.h"

#include "service_harness.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::services {
namespace {

using testing::ServiceHarness;
namespace ipc = kernel::ipc;

// dsp::DSP command headers. Source: 3dbrew, DSP Services.
constexpr std::uint32_t ConvertProcessAddressFromDspDram = 0x000C0040;
constexpr std::uint32_t WriteProcessPipe = 0x000D0082;
constexpr std::uint32_t GetPipeReadableSize = 0x000F0080;
constexpr std::uint32_t ReadPipeIfPossible = 0x001000C0;
constexpr std::uint32_t RegisterInterruptEvents = 0x00150082;

// The interrupt type for a pipe with a message waiting.
constexpr std::uint32_t MessageInterrupt = 2;
constexpr std::uint32_t Message = ServiceHarness::BufferAddress;
constexpr std::uint32_t Received = ServiceHarness::BufferAddress + 0x100;

// The start-up reply: a count of fifteen and fifteen DSP word addresses.
constexpr std::uint32_t StartupReplySize = 2 + (15 * 2);

struct Harness {
    Harness() : service(harness.Kernel()) {}

    std::shared_ptr<kernel::Event> RegisterAudioEvent() {
        auto event = std::make_shared<kernel::Event>(kernel::ResetType::OneShot);
        const kernel::Handle handle = harness.Kernel().Handles().Create(event).value_or(0);
        EXPECT_TRUE(harness.Send(service,
                                 {RegisterInterruptEvents,
                                  MessageInterrupt,
                                  DigitalSignalProcessor::AudioPipe,
                                  ipc::CopyHandlesDescriptor(1),
                                  handle}));
        return event;
    }

    bool SendAudioCommand(std::uint8_t command) {
        harness.WriteBytes(Message, std::vector<std::uint8_t>{command, 0, 0, 0});
        return harness.Send(service,
                            {WriteProcessPipe,
                             DigitalSignalProcessor::AudioPipe,
                             4,
                             ipc::StaticBufferDescriptorFor(4, 1),
                             Message});
    }

    // A request to the AAC decoder: codec 1, a command, and in a frame
    // request the size of the data at word 3 of the message.
    bool SendDecoderRequest(std::uint16_t command, std::uint32_t size) {
        std::vector<std::uint8_t> message(DigitalSignalProcessor::BinaryMessageSize, 0);
        message.at(0) = 1;
        message.at(2) = static_cast<std::uint8_t>(command);
        constexpr std::uint8_t Garbage = 0xEE;
        constexpr std::size_t RequestSize = 12;
        message.at(4) = Garbage;  // in the result field, as games leave it
        message.at(RequestSize) = static_cast<std::uint8_t>(size);
        message.at(RequestSize + 1) = static_cast<std::uint8_t>(size >> 8U);
        harness.WriteBytes(Message, message);
        return harness.Send(
            service,
            {WriteProcessPipe,
             DigitalSignalProcessor::BinaryPipe,
             DigitalSignalProcessor::BinaryMessageSize,
             ipc::StaticBufferDescriptorFor(DigitalSignalProcessor::BinaryMessageSize, 1),
             Message});
    }

    std::vector<std::uint8_t> ReadBinaryPipe() {
        constexpr auto Size = static_cast<std::uint32_t>(DigitalSignalProcessor::BinaryMessageSize);
        harness.ListReceiveBuffer(0, Received, Size);
        EXPECT_TRUE(harness.Send(
            service, {ReadPipeIfPossible, DigitalSignalProcessor::BinaryPipe, 0, Size}));
        return harness.ReadBytes(Received, Size);
    }

    ServiceHarness harness;
    DigitalSignalProcessor service;
};

TEST(DigitalSignalProcessor, ReportsItsStructuresWhenStarted) {
    Harness dsp;
    const auto event = dsp.RegisterAudioEvent();
    ASSERT_TRUE(dsp.SendAudioCommand(0));
    EXPECT_TRUE(dsp.service.IsOn());
    EXPECT_TRUE(event->signaled);

    ASSERT_TRUE(
        dsp.harness.Send(dsp.service, {GetPipeReadableSize, DigitalSignalProcessor::AudioPipe, 0}));
    EXPECT_EQ(dsp.harness.Reply(2), StartupReplySize);

    dsp.harness.ListReceiveBuffer(0, Received, StartupReplySize);
    ASSERT_TRUE(dsp.harness.Send(
        dsp.service, {ReadPipeIfPossible, DigitalSignalProcessor::AudioPipe, 0, StartupReplySize}));
    EXPECT_EQ(dsp.harness.Reply(2), StartupReplySize);
    EXPECT_EQ(dsp.harness.ReadBytes(Received, 4), (std::vector<std::uint8_t>{15, 0, 0xFF, 0xBF}))
        << "fifteen structures, the frame counter first at DSP word 0xBFFF";
    EXPECT_EQ(dsp.service.Readable(DigitalSignalProcessor::AudioPipe), 0U);
}

TEST(DigitalSignalProcessor, RaisesTheAudioInterruptEachFrameWhileOn) {
    Harness dsp;
    const auto event = dsp.RegisterAudioEvent();
    ASSERT_TRUE(dsp.SendAudioCommand(0));
    event->signaled = false;
    dsp.harness.Kernel().Threads().Advance(DigitalSignalProcessor::FrameTicks);
    EXPECT_TRUE(event->signaled);

    constexpr std::uint8_t Shutdown = 1;
    ASSERT_TRUE(dsp.SendAudioCommand(Shutdown));
    event->signaled = false;
    dsp.harness.Kernel().Threads().Advance(DigitalSignalProcessor::FrameTicks);
    EXPECT_FALSE(event->signaled);
}

TEST(DigitalSignalProcessor, EchoesTheDecodersInitialisationWithSuccess) {
    Harness dsp;
    ASSERT_TRUE(dsp.SendDecoderRequest(0, 0));
    const std::vector<std::uint8_t> reply = dsp.ReadBinaryPipe();
    EXPECT_EQ(reply.at(0), 1U) << "the codec";
    EXPECT_EQ(reply.at(4), 0U) << "success";
}

TEST(DigitalSignalProcessor, AnswersAFrameAsDecoded) {
    constexpr std::uint32_t FrameBytes = 0x180;
    Harness dsp;
    ASSERT_TRUE(dsp.SendDecoderRequest(1, FrameBytes));
    const std::vector<std::uint8_t> reply = dsp.ReadBinaryPipe();
    EXPECT_EQ(reply.at(8), 0U) << "48 kHz";
    EXPECT_EQ(reply.at(12), 2U) << "two channels";
    EXPECT_EQ(core::ReadU32At(reply, 16).ValueOr(0), FrameBytes);
    EXPECT_EQ(core::ReadU32At(reply, 28).ValueOr(0), 1024U) << "one frame of samples";
}

TEST(DigitalSignalProcessor, DeclinesAnUnknownAudioCommand) {
    Harness dsp;
    EXPECT_FALSE(dsp.SendAudioCommand(0x7F));
}

TEST(DigitalSignalProcessor, ConvertsADspWordAddressToTheProcessView) {
    Harness dsp;
    ASSERT_TRUE(dsp.harness.Send(dsp.service, {ConvertProcessAddressFromDspDram, 0xBFFF}));
    EXPECT_EQ(dsp.harness.Reply(2), 0x1FF40000U + (0xBFFFU << 1U));
}

}  // namespace
}  // namespace hexlith::domain::services
