#include "saru/core.h"
#include "saru/runtime_definitions.h"

namespace saru {

Result<bool> AsyncHandle::is_ready() const {
  Result<bool> res;

  saru::HostError err;
  bool ret;
  if (!saru_async_io_is_ready(this->handle, &ret, &err)) {
    res.emplace_err(err);
  } else {
    res.emplace(ret);
  }

  return res;
}

Result<std::optional<uint32_t>> AsyncHandle::select(const std::vector<AsyncHandle> &handles,
                                                    uint32_t timeout_ms) {
  Result<std::optional<uint32_t>> res;

  uint32_t ret = 0;
  saru::HostError err;
  if (!saru_async_io_select(handles.data(), handles.size(), timeout_ms, &ret, &err)) {
    res.emplace_err(err);
  } else if (ret != 0) {
    res.emplace(ret);
  } else {
    res.emplace(std::nullopt);
  }

  return res;
}

Result<HttpBody> HttpBody::make() {
  Result<HttpBody> res;

  uint32_t handle;
  saru::HostError err;
  if (!saru_http_body_new(&handle, &err)) {
    res.emplace_err(err);
  } else {
    res.emplace(handle);
  }

  return res;
}

Result<saru::String> HttpBody::read(uint32_t chunk_size) const {
  Result<saru::String> res;

  char *ret;
  size_t ret_len;
  saru::HostError err;
  if (!saru_http_body_read(this->handle, chunk_size, &ret, &ret_len, &err)) {
    res.emplace_err(err);
  } else {
    res.emplace(JS::UniqueChars(ret), ret_len);
  }

  return res;
}

Result<uint32_t> HttpBody::write(const uint8_t *ptr, size_t len) const {
  Result<uint32_t> res;

  saru::HostError err;
  uint32_t written;
  if (!saru_http_body_write(this->handle, ptr, len, SARU_HTTP_BODY_WRITE_END_BACK, &written,
                            &err)) {
    res.emplace_err(err);
  } else {
    res.emplace(written);
  }

  return res;
}

Result<Void> HttpBody::write_all(const uint8_t *ptr, size_t len) const {
  while (len > 0) {
    auto write_res = this->write(ptr, len);
    if (auto *err = write_res.to_err()) {
      return Result<Void>::err(*err);
    }

    auto written = write_res.unwrap();
    ptr += written;
    len -= std::min(len, static_cast<size_t>(written));
  }

  return Result<Void>::ok();
}

Result<Void> HttpBody::append(HttpBody other) const {
  Result<Void> res;

  saru::HostError err;
  if (!saru_http_body_append(this->handle, other.handle, &err)) {
    res.emplace_err(err);
  } else {
    res.emplace();
  }

  return res;
}

Result<Void> HttpBody::close() {
  Result<Void> res;

  saru::HostError err;
  if (!saru_http_body_close(this->handle, &err)) {
    res.emplace_err(err);
  } else {
    res.emplace();
  }

  return res;
}

AsyncHandle HttpBody::async_handle() const { return AsyncHandle{this->handle}; }

} // namespace saru