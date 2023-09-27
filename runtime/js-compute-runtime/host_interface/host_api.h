#ifndef JS_COMPUTE_RUNTIME_HOST_API_H
#define JS_COMPUTE_RUNTIME_HOST_API_H

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "saru/allocator.h"
#include "js/TypeDecls.h"
#include "saru/core.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#include "js/Utility.h"
#include "jsapi.h"
#pragma clang diagnostic pop

namespace host_api {

using saru::AsyncHandle;
using saru::Error;
using saru::HttpBody;
using saru::Result;
using saru::Void;

/// Generate an error in the JSContext.
void handle_fastly_error(JSContext *cx, Error err, int line, const char *func);

bool error_is_generic(Error e);
bool error_is_invalid_argument(Error e);
bool error_is_optional_none(Error e);
bool error_is_bad_handle(Error e);

struct HostBytes final {
  std::unique_ptr<uint8_t[]> ptr;
  size_t len;

  HostBytes() = default;
  HostBytes(std::nullptr_t) : HostBytes() {}
  HostBytes(std::unique_ptr<uint8_t[]> ptr, size_t len) : ptr{std::move(ptr)}, len{len} {}

  HostBytes(const HostBytes &other) = delete;
  HostBytes &operator=(const HostBytes &other) = delete;

  HostBytes(HostBytes &&other) : ptr{std::move(other.ptr)}, len{other.len} {}
  HostBytes &operator=(HostBytes &&other) {
    this->ptr.reset(other.ptr.release());
    this->len = other.len;
    return *this;
  }

  /// Allocate a zeroed HostBytes with the given number of bytes.
  static HostBytes with_capacity(size_t len) {
    HostBytes ret;
    ret.ptr = std::make_unique<uint8_t[]>(len);
    ret.len = len;
    return ret;
  }

  using iterator = uint8_t *;
  using const_iterator = const uint8_t *;

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

  /// Converstion to a `std::span<uint8_t>`.
  operator std::span<uint8_t>() const { return std::span{this->ptr.get(), this->len}; }
};

struct Response;

class HttpPendingReq final {
public:
  using Handle = uint32_t;

  static constexpr Handle invalid = UINT32_MAX - 1;

  Handle handle = invalid;

  HttpPendingReq() = default;
  explicit HttpPendingReq(Handle handle) : handle{handle} {}
  explicit HttpPendingReq(AsyncHandle async) : handle{async.handle} {}

  /// Poll for the response to this request.
  Result<std::optional<Response>> poll();

  /// Block until the response is ready.
  Result<Response> wait();

  /// Fetch the AsyncHandle for this pending request.
  AsyncHandle async_handle() const;
};

using HttpVersion = uint8_t;

class HttpBase {
public:
  virtual ~HttpBase() = default;

  virtual bool is_valid() const = 0;

  /// Get the http version used for this request.
  virtual Result<HttpVersion> get_version() const = 0;

  virtual Result<std::vector<saru::String>> get_header_names() = 0;
  virtual Result<std::optional<std::vector<saru::String>>>
  get_header_values(std::string_view name) = 0;
  virtual Result<Void> insert_header(std::string_view name, std::string_view value) = 0;
  virtual Result<Void> append_header(std::string_view name, std::string_view value) = 0;
  virtual Result<Void> remove_header(std::string_view name) = 0;
};

struct TlsVersion {
  uint8_t value = 0;

  explicit TlsVersion(uint8_t raw);

  static TlsVersion version_1();
  static TlsVersion version_1_1();
  static TlsVersion version_1_2();
  static TlsVersion version_1_3();
};

struct BackendConfig {
  std::optional<saru::String> host_override;
  std::optional<uint32_t> connect_timeout;
  std::optional<uint32_t> first_byte_timeout;
  std::optional<uint32_t> between_bytes_timeout;
  std::optional<bool> use_ssl;
  std::optional<bool> dont_pool;
  std::optional<TlsVersion> ssl_min_version;
  std::optional<TlsVersion> ssl_max_version;
  std::optional<saru::String> cert_hostname;
  std::optional<saru::String> ca_cert;
  std::optional<saru::String> ciphers;
  std::optional<saru::String> sni_hostname;
};

struct CacheOverrideTag final {
  uint8_t value = 0;

  void set_pass();
  void set_ttl();
  void set_stale_while_revalidate();
  void set_pci();
};

struct Request;

class HttpReq final : public HttpBase {
public:
  using Handle = uint32_t;

  static constexpr Handle invalid = UINT32_MAX - 1;

  Handle handle = invalid;

  HttpReq() = default;
  explicit HttpReq(Handle handle) : handle{handle} {}

  static Result<HttpReq> make();

  static Result<Void> redirect_to_grip_proxy(std::string_view backend);

  static Result<Void> register_dynamic_backend(std::string_view name, std::string_view target,
                                               const BackendConfig &config);

  /// Fetch the downstream request/body pair
  static Result<Request> downstream_get();

  /// Get the downstream ip address.
  static Result<HostBytes> downstream_client_ip_addr();

  static Result<saru::String> http_req_downstream_tls_cipher_openssl_name();

  static Result<saru::String> http_req_downstream_tls_protocol();

  static Result<HostBytes> http_req_downstream_tls_client_hello();

  static Result<HostBytes> http_req_downstream_tls_raw_client_certificate();

  static Result<HostBytes> http_req_downstream_tls_ja3_md5();

  Result<Void> auto_decompress_gzip();

  /// Send this request synchronously, and wait for the response.
  Result<Response> send(HttpBody body, std::string_view backend);

  /// Send this request asynchronously.
  Result<HttpPendingReq> send_async(HttpBody body, std::string_view backend);

  /// Send this request asynchronously, and allow sending additional data through the body.
  Result<HttpPendingReq> send_async_streaming(HttpBody body, std::string_view backend);

  /// Get the http version used for this request.

  /// Set the request method.
  Result<Void> set_method(std::string_view method);

  /// Get the request method.
  Result<saru::String> get_method() const;

  /// Set the request uri.
  Result<Void> set_uri(std::string_view str);

  /// Get the request uri.
  Result<saru::String> get_uri() const;

  /// Configure cache-override settings.
  Result<Void> cache_override(CacheOverrideTag tag, std::optional<uint32_t> ttl,
                              std::optional<uint32_t> stale_while_revalidate,
                              std::optional<std::string_view> surrogate_key);

  bool is_valid() const override;

  Result<HttpVersion> get_version() const override;

  Result<std::vector<saru::String>> get_header_names() override;
  Result<std::optional<std::vector<saru::String>>>
  get_header_values(std::string_view name) override;
  Result<Void> insert_header(std::string_view name, std::string_view value) override;
  Result<Void> append_header(std::string_view name, std::string_view value) override;
  Result<Void> remove_header(std::string_view name) override;
};

class HttpResp final : public HttpBase {
public:
  using Handle = uint32_t;

  static constexpr Handle invalid = UINT32_MAX - 1;

  Handle handle = invalid;

  HttpResp() = default;
  explicit HttpResp(Handle handle) : handle{handle} {}

  static Result<HttpResp> make();

  /// Get the http status for the response.
  Result<uint16_t> get_status() const;

  /// Set the http status for the response.
  Result<Void> set_status(uint16_t status);

  /// Immediately begin sending this response to the downstream client.
  Result<Void> send_downstream(HttpBody body, bool streaming);

  bool is_valid() const override;

  Result<HttpVersion> get_version() const override;

  Result<std::vector<saru::String>> get_header_names() override;
  Result<std::optional<std::vector<saru::String>>>
  get_header_values(std::string_view name) override;
  Result<Void> insert_header(std::string_view name, std::string_view value) override;
  Result<Void> append_header(std::string_view name, std::string_view value) override;
  Result<Void> remove_header(std::string_view name) override;
};

/// The pair of a response and its body.
struct Response {
  HttpResp resp;
  HttpBody body;

  Response() = default;
  Response(HttpResp resp, HttpBody body) : resp{resp}, body{body} {}
};

/// The pair of a request and its body.
struct Request {
  HttpReq req;
  HttpBody body;

  Request() = default;
  Request(HttpReq req, HttpBody body) : req{req}, body{body} {}
};

class GeoIp final {
  ~GeoIp() = delete;

public:
  /// Lookup information about the ip address provided.
  static Result<saru::String> lookup(std::span<uint8_t> bytes);
};

class LogEndpoint final {
public:
  using Handle = uint32_t;

  Handle handle = UINT32_MAX - 1;

  LogEndpoint() = default;
  explicit LogEndpoint(Handle handle) : handle{handle} {}

  static Result<LogEndpoint> get(std::string_view name);

  Result<Void> write(std::string_view msg);
};

class Dict final {
public:
  using Handle = uint32_t;

  Handle handle = UINT32_MAX - 1;

  Dict() = default;
  explicit Dict(Handle handle) : handle{handle} {}

  static Result<Dict> open(std::string_view name);

  Result<std::optional<saru::String>> get(std::string_view name);
};

class ObjectStore final {
public:
  using Handle = uint32_t;

  Handle handle = UINT32_MAX - 1;

  ObjectStore() = default;
  explicit ObjectStore(Handle handle) : handle{handle} {}

  static Result<ObjectStore> open(std::string_view name);

  Result<std::optional<HttpBody>> lookup(std::string_view name);

  Result<Void> insert(std::string_view name, HttpBody body);
};

class Secret final {
public:
  using Handle = uint32_t;

  Handle handle = UINT32_MAX - 1;

  Secret() = default;
  explicit Secret(Handle handle) : handle{handle} {}

  Result<std::optional<saru::String>> plaintext() const;
};

class SecretStore final {
public:
  using Handle = uint32_t;

  Handle handle = UINT32_MAX - 1;

  SecretStore() = default;
  explicit SecretStore(Handle handle) : handle{handle} {}

  static Result<SecretStore> open(std::string_view name);

  Result<std::optional<Secret>> get(std::string_view name);
};

class Random final {
public:
  static Result<HostBytes> get_bytes(size_t num_bytes);

  static Result<uint32_t> get_u32();
};

struct CacheLookupOptions final {
  /// A full request handle, used only for its headers.
  HttpReq request_headers;
};

struct CacheGetBodyOptions final {
  uint64_t start = 0;
  uint64_t end = 0;
};

struct CacheWriteOptions final {
  uint64_t max_age_ns = 0;
  HttpReq req;
  std::string_view vary_rule;

  uint64_t initial_age_ns = 0;
  uint64_t stale_while_revalidate_ns = 0;

  std::string_view surrogate_keys;

  uint64_t length = 0;

  std::span<uint8_t> metadata;

  bool sensitive = false;
};

struct CacheState final {
  uint8_t state = 0;

  CacheState() = default;
  CacheState(uint8_t state) : state{state} {}

  bool is_found() const;
  bool is_usable() const;
  bool is_stale() const;
  bool must_insert_or_update() const;
};

class CacheHandle final {
public:
  using Handle = uint32_t;

  static constexpr Handle invalid = UINT32_MAX - 1;

  Handle handle = invalid;

  CacheHandle() = default;
  explicit CacheHandle(Handle handle) : handle{handle} {}

  /// Lookup a cached object.
  static Result<CacheHandle> lookup(std::string_view key, const CacheLookupOptions &opts);

  static Result<CacheHandle> transaction_lookup(std::string_view key,
                                                const CacheLookupOptions &opts);

  /// Insert a cache object.
  static Result<HttpBody> insert(std::string_view key, const CacheWriteOptions &opts);

  /// Insert this cached object and stream it back.
  Result<std::tuple<HttpBody, CacheHandle>> insert_and_stream_back(const CacheWriteOptions &opts);

  bool is_valid() const { return this->handle != invalid; }

  /// Cancel a transaction.
  Result<Void> transaction_cancel();

  /// Fetch the body handle for the cached data.
  Result<HttpBody> get_body(const CacheGetBodyOptions &opts);

  /// Fetch the state for this cache handle.
  Result<CacheState> get_state();
};

class Fastly final {
  ~Fastly() = delete;

public:
  /// Purge the given surrogate key.
  static Result<std::optional<saru::String>> purge_surrogate_key(std::string_view key);
};

} // namespace host_api

#endif
