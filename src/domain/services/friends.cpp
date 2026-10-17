#include "domain/services/friends.h"

#include <array>
#include <vector>

namespace hexlith::domain::services {
namespace {

// A friend key is a principal ID, an unknown word and a 64-bit friend code;
// the presence block is 0x12C bytes nobody has decoded. Source: FriendKey and
// MyPresence in Azahar's frd.h.
constexpr std::uint32_t FriendKeyWords = 4;
constexpr std::size_t FriendKeySize = 0x10;
constexpr std::size_t PresenceSize = 0x12C;

}  // namespace

std::span<const Friends::Command> Friends::Commands() {
    static constexpr std::array<Command, 12> Table = {{
        {.id = 0x0001, .name = "HasLoggedIn", .handler = &Friends::HasLoggedIn},
        {.id = 0x0002, .name = "IsOnline", .handler = &Friends::HasLoggedIn},
        {.id = 0x0003, .name = "Login"},
        {.id = 0x0005, .name = "GetMyFriendKey", .handler = &Friends::GetMyFriendKey},
        {.id = 0x0006, .name = "GetMyPreference"},
        {.id = 0x0007, .name = "GetMyProfile"},
        {.id = 0x0008, .name = "GetMyPresence", .handler = &Friends::GetMyPresence},
        {.id = 0x0009, .name = "GetMyScreenName"},
        {.id = 0x000A, .name = "GetMyMii"},
        {.id = 0x0011, .name = "GetFriendKeyList", .handler = &Friends::GetFriendKeyList},
        {.id = 0x0021, .name = "SetNotificationMask"},
        {.id = 0x0032, .name = "SetClientSdkVersion", .handler = &Friends::SetClientSdkVersion},
    }};
    return Table;
}

void Friends::HasLoggedIn(kernel::IpcRequest& request) {
    request.Begin(2, 0);
    request.Push(kernel::result::Success);
    request.Push(0);
}

void Friends::GetMyFriendKey(kernel::IpcRequest& request) {
    request.Begin(1 + FriendKeyWords, 0);
    request.Push(kernel::result::Success);
    for (std::uint32_t word = 0; word < FriendKeyWords; ++word) {
        request.Push(0);
    }
}

void Friends::GetMyPresence(kernel::IpcRequest& request) {
    const std::vector<std::uint8_t> presence(PresenceSize, 0);
    request.Begin(1, 2);
    request.Push(kernel::result::Success);
    request.PushStaticBuffer(presence, 0);
}

void Friends::GetFriendKeyList(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());
    const std::uint32_t count = request.Pop();
    const std::vector<std::uint8_t> keys(FriendKeySize * count, 0);
    request.Begin(2, 2);
    request.Push(kernel::result::Success);
    request.Push(0);  // no friends
    request.PushStaticBuffer(keys, 0);
}

void Friends::SetClientSdkVersion(kernel::IpcRequest& request) {
    static_cast<void>(request.Pop());
    static_cast<void>(request.PopProcessId());
    request.ReplyWith(kernel::result::Success);
}

}  // namespace hexlith::domain::services
