#include "application/golden_trace.h"

#include <gtest/gtest.h>

namespace hexlith::application {
namespace {

using ModuleEvent = domain::services::RelocatableObjects::ModuleEvent;

TEST(GoldenTrace, NamesItsFormatOnTheFirstLine) {
    EXPECT_EQ(GoldenTraceHeader(), "# hexlith golden trace, format 1");
}

TEST(GoldenTrace, WritesARequestAsFrameServiceAndHeader) {
    constexpr std::uint64_t Frame = 12;
    constexpr std::uint32_t Header = 0x000C0000;
    EXPECT_EQ(GoldenRequestLine(Frame, "gsp::Gpu", Header), "12 ipc gsp::Gpu 000C0000");
}

TEST(GoldenTrace, WritesAModuleEventWithItsAddress) {
    constexpr std::uint64_t Frame = 5;
    constexpr std::uint32_t Address = 0x006A5000;
    const ModuleEvent loaded{.kind = ModuleEvent::Kind::Loaded, .name = "DllA", .address = Address};
    EXPECT_EQ(GoldenModuleLine(Frame, loaded), "5 cro-load DllA 006A5000");
    const ModuleEvent unloaded{
        .kind = ModuleEvent::Kind::Unloaded, .name = "DllA", .address = Address};
    EXPECT_EQ(GoldenModuleLine(Frame, unloaded), "5 cro-unload DllA 006A5000");
}

}  // namespace
}  // namespace hexlith::application
