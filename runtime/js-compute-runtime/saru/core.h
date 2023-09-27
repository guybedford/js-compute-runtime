#ifndef SARU_CORE_H
#define SARU_CORE_H

#include "js/TypeDecls.h"
#include "saru/builtin.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#include "js/Utility.h"
#include "jsapi.h"
#pragma clang diagnostic pop

namespace saru {

/// The type of erros returned from the host.
using HostError = uint8_t;

/// A type to signal that a result produces no value.
struct Void final {};

template <typename T> class Result final {
  struct Error {
    HostError value;

    explicit Error(HostError value) : value{value} {}
  };

  std::variant<T, Error> result;

public:
  Result() = default;

  /// Explicitly construct an error.
  static Result err(HostError err) {
    Result res;
    res.emplace_err(err);
    return res;
  }

  /// Explicitly construct a successful result.
  template <typename... Args> static Result ok(Args &&...args) {
    Result res;
    res.emplace(std::forward<Args>(args)...);
    return res;
  }

  /// Construct an error in-place.
  HostError &emplace_err(HostError err) & {
    return this->result.template emplace<Error>(err).value;
  }

  /// Construct a value of T in-place.
  template <typename... Args> T &emplace(Args &&...args) {
    return this->result.template emplace<T>(std::forward<Args>(args)...);
  }

  /// True when the result contains an error.
  bool is_err() const { return std::holds_alternative<Error>(this->result); }

  /// Return a pointer to the error value of this result, if the call failed.
  const HostError *to_err() const {
    return reinterpret_cast<const HostError *>(std::get_if<Error>(&this->result));
  }

  /// Assume the call was successful, and return a reference to the result.
  T &unwrap() { return std::get<T>(this->result); }
};

/// A string allocated by the host interface. Holds ownership of the data.
struct String final {
  JS::UniqueChars ptr;
  size_t len;

  String() = default;
  String(std::nullptr_t) : String() {}
  String(JS::UniqueChars ptr, size_t len) : ptr{std::move(ptr)}, len{len} {}

  String(const String &other) = delete;
  String &operator=(const String &other) = delete;

  String(String &&other) : ptr{std::move(other.ptr)}, len{other.len} {}
  String &operator=(saru::String &&other) {
    this->ptr.reset(other.ptr.release());
    this->len = other.len;
    return *this;
  }

  using iterator = char *;
  using const_iterator = const char *;

  size_t size() const { return this->len; }

  iterator begin() { return this->ptr.get(); }
  iterator end() { return this->begin() + this->len; }

  const_iterator begin() const { return this->ptr.get(); }
  const_iterator end() const { return this->begin() + this->len; }

  /// Conversion to a bool, testing for an empty pointer.
  operator bool() const { return this->ptr != nullptr; }

  /// Comparison against nullptr
  bool operator==(std::nullptr_t) { return this->ptr == nullptr; }

  /// Comparison against nullptr
  bool operator!=(std::nullptr_t) { return this->ptr != nullptr; }

  /// Conversion to a `std::string_view`.
  operator std::string_view() const { return std::string_view(this->ptr.get(), this->len); }
};

/// Common methods for async handles.
class AsyncHandle {
public:
  using Handle = uint32_t;

  static constexpr Handle invalid = UINT32_MAX - 1;

  Handle handle;

  AsyncHandle() = default;
  explicit AsyncHandle(Handle handle) : handle{handle} {}

  /// Check to see if this handle is ready.
  Result<bool> is_ready() const;

  /// Return the index in handles of the `AsyncHandle` that's ready. If the select call finishes
  /// successfully and returns `std::nullopt`, the timeout has expired.
  ///
  /// If the timeout is `0`, two behaviors are possible
  ///   * if handles is empty, an error will be returned immediately
  ///   * otherwise, block until a handle is ready and return its index
  ///
  /// If the timeout is non-zero, two behaviors are possible
  ///   * no handle becomes ready within timeout, and the successful `std::nullopt` is returned
  ///   * a handle becomes ready within the timeout, and its index is returned.
  static Result<std::optional<uint32_t>> select(const std::vector<AsyncHandle> &handles,
                                                uint32_t timeout_ms);
};

/// A convenience wrapper for the host calls involving http bodies.
class HttpBody final {
public:
  using Handle = uint32_t;

  static constexpr Handle invalid = UINT32_MAX - 1;

  /// The handle to use when making host calls, initialized to the special invalid value used by
  /// executed.
  Handle handle = invalid;

  HttpBody() = default;
  explicit HttpBody(Handle handle) : handle{handle} {}
  explicit HttpBody(AsyncHandle async) : handle{async.handle} {}

  /// Returns true when this body handle is valid.
  bool valid() const { return this->handle != invalid; }

  /// Make a new body handle.
  static Result<HttpBody> make();

  /// Read a chunk from this handle.
  Result<saru::String> read(uint32_t chunk_size) const;

  /// Write a chunk to this handle.
  Result<uint32_t> write(const uint8_t *bytes, size_t len) const;

  /// Writes the given number of bytes from the given buffer to the given handle.
  ///
  /// The host doesn't necessarily write all bytes in any particular call to
  /// `write`, so to ensure all bytes are written, we call it in a loop.
  Result<Void> write_all(const uint8_t *bytes, size_t len) const;

  /// Append another HttpBody to this one.
  Result<Void> append(HttpBody other) const;

  /// Close this handle, and reset internal state to invalid.
  Result<Void> close();

  AsyncHandle async_handle() const;
};

} // namespace saru

#endif