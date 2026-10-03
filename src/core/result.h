#ifndef HEXLITH_CORE_RESULT_H
#define HEXLITH_CORE_RESULT_H

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hexlith::core {

// Why an operation failed, as a sentence a person can read.
//
// Parsers nest, so the message a failure carries where it is detected is
// rarely enough on its own: "offset 0x1b0 is past the end of 0x200 bytes"
// does not say which header was being read. WithContext prepends the caller's
// description as the failure travels outwards, which turns that into
// "NCCH header: offset 0x1b0 is past the end of 0x200 bytes".
class Error {
public:
    explicit Error(std::string message) : message_(std::move(message)) {}

    [[nodiscard]] const std::string& Message() const {
        return message_;
    }

    [[nodiscard]] Error WithContext(std::string_view context) const {
        std::string combined;
        combined.reserve(context.size() + 2 + message_.size());
        combined.append(context);
        combined.append(": ");
        combined.append(message_);
        return Error{std::move(combined)};
    }

private:
    std::string message_;
};

// Either a value or the reason there is none. This is the std::expected that
// C++20 does not have; ADR 0001 records why the standard is not raised to
// C++23 for it.
//
// The interface is deliberately narrow. There is no monadic chaining and no
// propagation macro, because an explicit check at each step is what keeps a
// parser readable six months later.
//
// Both constructors are implicit, so a function returning Result<Header> can
// `return header;` or `return Error{...};` without naming the type again.
//
// Value and Failure on the wrong alternative are programming mistakes rather
// than runtime conditions, so they throw std::logic_error instead of
// returning something plausible.
template<typename T>
class Result {
public:
    Result(T value) : contents_(std::move(value)) {}

    Result(Error error) : contents_(std::move(error)) {}

    [[nodiscard]] bool Ok() const {
        return std::holds_alternative<T>(contents_);
    }

    explicit operator bool() const {
        return Ok();
    }

    [[nodiscard]] const T& Value() const {
        RequireOk();
        return std::get<T>(contents_);
    }

    [[nodiscard]] T& Value() {
        RequireOk();
        return std::get<T>(contents_);
    }

    [[nodiscard]] T ValueOr(T fallback) const {
        return Ok() ? std::get<T>(contents_) : std::move(fallback);
    }

    [[nodiscard]] const Error& Failure() const {
        RequireFailed();
        return std::get<Error>(contents_);
    }

private:
    void RequireOk() const {
        if (!Ok()) {
            throw std::logic_error("Result::Value called on a failed result: " +
                                   std::get<Error>(contents_).Message());
        }
    }

    void RequireFailed() const {
        if (Ok()) {
            throw std::logic_error("Result::Failure called on a result that holds a value");
        }
    }

    std::variant<T, Error> contents_;
};

// An operation that either succeeds or explains why it did not. Spelt as a
// specialisation rather than as Result<Unit> so that call sites read the same
// way whether or not there is a value to carry.
template<>
class Result<void> {
public:
    Result() = default;

    Result(Error error) : failure_(std::move(error)) {}

    [[nodiscard]] bool Ok() const {
        return !failure_.has_value();
    }

    explicit operator bool() const {
        return Ok();
    }

    // The guard tests the optional directly rather than calling Ok, so that a
    // static analyser can see that the dereference below is reachable only
    // when there is something to dereference.
    [[nodiscard]] const Error& Failure() const {
        if (!failure_.has_value()) {
            throw std::logic_error("Result::Failure called on a result that holds a value");
        }
        return *failure_;
    }

private:
    std::optional<Error> failure_;
};

using Status = Result<void>;

}  // namespace hexlith::core

#endif  // HEXLITH_CORE_RESULT_H
