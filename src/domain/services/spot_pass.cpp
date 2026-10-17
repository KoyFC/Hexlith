#include "domain/services/spot_pass.h"

#include <array>

namespace hexlith::domain::services {
namespace {

// RegisterStorageEntry carries five words whose meaning 3dbrew does not give.
constexpr int StorageEntryWords = 5;

}  // namespace

std::span<const SpotPass::Command> SpotPass::Commands() {
    static constexpr std::array<Command, 6> Table = {{
        {.id = 0x0001, .name = "InitializeSession", .handler = &SpotPass::InitializeSession},
        {.id = 0x0002, .name = "SetStorageInfo"},
        {.id = 0x0003, .name = "UnregisterStorage", .handler = &SpotPass::UnregisterStorage},
        {.id = 0x0004, .name = "GetStorageInfo"},
        {.id = 0x002F, .name = "RegisterStorageEntry", .handler = &SpotPass::RegisterStorageEntry},
        {.id = 0x0030, .name = "GetStorageEntryInfo", .handler = &SpotPass::GetStorageEntryInfo},
    }};
    return Table;
}

void SpotPass::InitializeSession(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop64());  // the program ID, zero for the caller itself
    static_cast<void>(request.PopProcessId());
    session_ = true;
    request.ReplyWith(kernel::result::Success);
}

void SpotPass::UnregisterStorage(kernel::IpcRequest& request) {
    request.ReplyWith(kernel::result::Success);
}

void SpotPass::RegisterStorageEntry(kernel::IpcRequest& request) {
    for (int word = 0; word < StorageEntryWords; ++word) {
        static_cast<void>(request.Pop());
    }
    request.ReplyWith(kernel::result::Success);
}

void SpotPass::GetStorageEntryInfo(kernel::IpcRequest& request) {
    // Two values, a word and a halfword, both zero when nothing is registered.
    request.Begin(3, 0);
    request.Push(kernel::result::Success);
    request.Push(0);
    request.Push(0);
}

}  // namespace hexlith::domain::services
