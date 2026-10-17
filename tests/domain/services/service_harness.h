#ifndef HEXLITH_TESTS_DOMAIN_SERVICES_SERVICE_HARNESS_H
#define HEXLITH_TESTS_DOMAIN_SERVICES_SERVICE_HARNESS_H

#include "domain/kernel/ipc.h"
#include "domain/kernel/kernel.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hexlith::domain::services::testing {

// A kernel with a main thread, whose thread-local storage holds the command
// buffer, and a page of private memory for the buffers a request names. A
// request goes to the service directly, as SendSyncRequest would hand it.
class ServiceHarness {
public:
    // Where the page of buffers sits, and how large it is.
    static constexpr std::uint32_t BufferAddress = 0x00200000;
    static constexpr std::uint32_t BufferSize = kernel::layout::PageSize;

    ServiceHarness();

    kernel::Kernel& Kernel() {
        return kernel_;
    }

    kernel::Thread& Caller();

    // Writes the request into the caller's command buffer, has the service
    // answer it and writes the reply back. Answers whether the service
    // implemented the command.
    bool Send(kernel::SessionHandler& service, const std::vector<std::uint32_t>& words);

    // A word of the last reply.
    std::uint32_t Reply(std::size_t index);

    // Lists a buffer the caller can receive static data into, as a thread does
    // at 0x180 in its local storage.
    void ListReceiveBuffer(std::uint32_t index, std::uint32_t address, std::uint32_t capacity);

    void WriteBytes(std::uint32_t address, std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> ReadBytes(std::uint32_t address, std::uint32_t size);
    std::uint32_t Read32(std::uint32_t address);

    // The object a handle in the reply names.
    template<typename T>
    std::shared_ptr<T> ReplyObject(std::size_t index, kernel::ObjectType type) {
        return kernel_.Handles().Get<T>(Reply(index), type);
    }

private:
    kernel::Kernel kernel_;
};

}  // namespace hexlith::domain::services::testing

#endif  // HEXLITH_TESTS_DOMAIN_SERVICES_SERVICE_HARNESS_H
