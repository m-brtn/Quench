#pragma once

#include <quench/core/Assert.h>

#include <optional>
#include <string>
#include <utility>

namespace quench {

// The failure of an operation the caller can handle: a message in English with the values that explain it.
struct Error {
    std::string message;
};

// A value or an Error. Engine APIs return this instead of throwing; the accessor names follow the engine style, so a
// later switch to std::expected is a rename.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_value(std::move(value)) {}
    Result(Error error) : m_error(std::move(error.message)) {}

    bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }

    T& value() & {
        QUENCH_ASSERT(ok(), "Result::value() on an error");
        return *m_value;
    }
    const T& value() const& {
        QUENCH_ASSERT(ok(), "Result::value() on an error");
        return *m_value;
    }
    T&& value() && {
        QUENCH_ASSERT(ok(), "Result::value() on an error");
        return std::move(*m_value);
    }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    T valueOr(T fallback) const& { return ok() ? *m_value : std::move(fallback); }

    // Empty when ok().
    const std::string& error() const { return m_error; }

private:
    std::optional<T> m_value;
    std::string m_error;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_ok(false), m_error(std::move(error.message)) {}

    bool ok() const { return m_ok; }
    explicit operator bool() const { return ok(); }
    const std::string& error() const { return m_error; }

private:
    bool m_ok = true;
    std::string m_error;
};

} // namespace quench
