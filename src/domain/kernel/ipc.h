#ifndef HEXLITH_DOMAIN_KERNEL_IPC_H
#define HEXLITH_DOMAIN_KERNEL_IPC_H

#include "domain/kernel/objects.h"
#include "domain/kernel/result_code.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::kernel {

class Kernel;

// The IPC command buffer, as 3dbrew's IPC page lays it out. docs/ipc.md has
// the whole of it.
namespace ipc {

// Where a thread's command buffer is in its thread-local storage, how many
// words it holds, and where the thread lists the buffers it can receive static
// data into.
constexpr std::uint32_t CommandBufferOffset = 0x80;
constexpr std::size_t CommandBufferWords = 64;
constexpr std::uint32_t ReceiveBufferListOffset = 0x180;
constexpr std::uint32_t ReceiveBufferEntrySize = 8;
constexpr std::uint32_t ReceiveBufferCount = 16;

// The header: command ID in bits 16 to 31, the count of normal words in bits 6
// to 11, the count of translate words in bits 0 to 5.
constexpr unsigned CommandShift = 16;
constexpr unsigned NormalShift = 6;
constexpr std::uint32_t CountMask = 0x3F;

[[nodiscard]] constexpr std::uint32_t
MakeHeader(std::uint32_t command, std::uint32_t normal, std::uint32_t translate) {
    return (command << CommandShift) | ((normal & CountMask) << NormalShift) |
           (translate & CountMask);
}

[[nodiscard]] constexpr std::uint32_t CommandOf(std::uint32_t header) {
    return header >> CommandShift;
}

[[nodiscard]] constexpr std::uint32_t NormalCountOf(std::uint32_t header) {
    return (header >> NormalShift) & CountMask;
}

[[nodiscard]] constexpr std::uint32_t TranslateCountOf(std::uint32_t header) {
    return header & CountMask;
}

// Translate descriptors. The type is bits 1 to 3; for handles, bit 4 moves
// them and bit 5 replaces them with the caller's process ID, and the count less
// one is in bits 26 to 31. A static buffer has its index in bits 10 to 13 and
// its size in bits 14 to 31. A mapped buffer sets bit 3, with its access in
// bits 1 and 2 and its size in bits 4 to 31.
constexpr std::uint32_t DescriptorTypeMask = 0xE;
constexpr std::uint32_t HandleDescriptor = 0x0;
constexpr std::uint32_t MoveHandlesFlag = 0x10;
constexpr std::uint32_t CallingProcessIdFlag = 0x20;
constexpr unsigned HandleCountShift = 26;
constexpr std::uint32_t StaticBufferDescriptor = 0x2;
constexpr unsigned StaticBufferIndexShift = 10;
constexpr std::uint32_t StaticBufferIndexMask = 0xF;
constexpr unsigned StaticBufferSizeShift = 14;
constexpr std::uint32_t MappedBufferFlag = 0x8;
constexpr unsigned MappedBufferAccessShift = 1;
constexpr std::uint32_t MappedBufferAccessMask = 0x3;
constexpr unsigned MappedBufferSizeShift = 4;

[[nodiscard]] constexpr std::uint32_t CopyHandlesDescriptor(std::uint32_t count) {
    return HandleDescriptor | ((count - 1) << HandleCountShift);
}

[[nodiscard]] constexpr std::uint32_t MoveHandlesDescriptor(std::uint32_t count) {
    return HandleDescriptor | MoveHandlesFlag | ((count - 1) << HandleCountShift);
}

constexpr std::uint32_t CallingProcessIdDescriptor = CallingProcessIdFlag;

[[nodiscard]] constexpr std::uint32_t StaticBufferDescriptorFor(std::uint32_t size,
                                                                std::uint32_t index) {
    return StaticBufferDescriptor | (index << StaticBufferIndexShift) |
           (size << StaticBufferSizeShift);
}

[[nodiscard]] constexpr std::uint32_t MappedBufferDescriptorFor(std::uint32_t size,
                                                                std::uint32_t access) {
    return MappedBufferFlag | (access << MappedBufferAccessShift) | (size << MappedBufferSizeShift);
}

// Answered when a request's descriptors are not the ones the command takes.
// Source: ResultInvalidBufferDescriptor in Azahar's errors.h.
constexpr ResultCode InvalidBufferDescriptor{0xD9001830};

}  // namespace ipc

// A buffer the caller passes by address, for the service to read or write in
// place.
struct MappedBuffer {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t access = 0;
};

// One request as a service sees it: the words the client wrote, read in order,
// and the reply the service builds, written back when the request completes.
// Handles the client sends arrive as objects, and objects the service returns
// become handles in the client's table. Modelled on Azahar's RequestParser and
// RequestBuilder, src/core/hle/ipc_helpers.h.
class IpcRequest {
public:
    IpcRequest(Kernel& kernel, Thread& thread);

    [[nodiscard]] std::uint32_t Header() const {
        return words_.at(0);
    }

    [[nodiscard]] std::uint32_t Command() const {
        return ipc::CommandOf(Header());
    }

    [[nodiscard]] Kernel& GetKernel() {
        return *kernel_;
    }

    [[nodiscard]] Thread& Caller() {
        return *thread_;
    }

    // Reading the request, word by word after the header. A read past what
    // the header declares, or a descriptor of the wrong kind, marks the request
    // malformed and answers zero or nothing.
    std::uint32_t Pop();
    std::uint64_t Pop64();
    std::vector<std::shared_ptr<Object>> PopHandles();
    std::uint32_t PopProcessId();
    MappedBuffer PopMappedBuffer();
    std::vector<std::uint8_t> PopStaticBuffer();

    [[nodiscard]] bool Malformed() const {
        return malformed_;
    }

    // Says the command is implemented but not for what this request asks,
    // such as a configuration block nothing here holds. The request is then
    // left unanswered, as a command with no implementation is, and the reason
    // goes into the report.
    void Decline(std::string reason) {
        declined_ = std::move(reason);
    }

    [[nodiscard]] const std::optional<std::string>& Declined() const {
        return declined_;
    }

    // Building the reply. Begin starts it with the same command ID; the first
    // normal word is always the result.
    void Begin(std::uint32_t normal_words, std::uint32_t translate_words);
    void Push(std::uint32_t word);

    void Push(ResultCode result) {
        Push(result.raw);
    }

    void Push64(std::uint64_t value);
    void PushCopyObjects(std::span<const std::shared_ptr<Object>> objects);
    void PushMoveObjects(std::span<const std::shared_ptr<Object>> objects);
    void PushMappedBuffer(const MappedBuffer& buffer);
    // Copies data into the buffer the client listed for that index, and
    // describes it in the reply.
    void PushStaticBuffer(std::span<const std::uint8_t> data, std::uint32_t index);

    // Answers with a result alone.
    void ReplyWith(ResultCode result) {
        Begin(1, 0);
        Push(result);
    }

    // Writes the reply into the client's command buffer.
    void Complete();

private:
    void PushHandles(std::span<const std::shared_ptr<Object>> objects, bool move);

    // Never null: a request exists only while its caller's system call does.
    Kernel* kernel_;
    Thread* thread_;
    std::array<std::uint32_t, ipc::CommandBufferWords> words_{};
    std::array<std::uint32_t, ipc::CommandBufferWords> reply_{};
    std::size_t read_ = 1;
    std::size_t written_ = 0;
    bool malformed_ = false;
    std::optional<std::string> declined_;
};

// What answers the requests sent on a session: a service, or an object a
// service handed out, such as an open file. A virtual interface, called once
// per request, at the boundary between the game and the host.
class SessionHandler {
public:
    SessionHandler() = default;
    SessionHandler(const SessionHandler&) = delete;
    SessionHandler& operator=(const SessionHandler&) = delete;
    SessionHandler(SessionHandler&&) = delete;
    SessionHandler& operator=(SessionHandler&&) = delete;
    virtual ~SessionHandler() = default;

    // The port name of the service, for reports.
    [[nodiscard]] virtual std::string_view Name() const = 0;

    // Answers a request. False when the command is not implemented, which
    // stops the run with the service's name and the command's header rather
    // than answering anything.
    virtual bool Handle(IpcRequest& request) = 0;

    // The name of a command, where it is known, for reports.
    [[nodiscard]] virtual std::string_view CommandName(std::uint32_t /*command*/) const {
        return {};
    }
};

// The client end of a session, which is what a service handle refers to.
class ClientSession final : public WaitObject {
public:
    explicit ClientSession(std::shared_ptr<SessionHandler> answers) : handler(std::move(answers)) {}

    [[nodiscard]] ObjectType Type() const override {
        return ObjectType::ClientSession;
    }

    // A client never waits on its own session; the server end is the one
    // waited on, and an HLE service has none.
    [[nodiscard]] bool ShouldWait(const Thread& /*thread*/) const override {
        return true;
    }

    void Acquire(Thread& /*thread*/) override {}

    std::shared_ptr<SessionHandler> handler;
};

}  // namespace hexlith::domain::kernel

#endif  // HEXLITH_DOMAIN_KERNEL_IPC_H
