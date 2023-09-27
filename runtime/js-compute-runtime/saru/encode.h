#ifndef SARU_CORE_ENCODE_H
#define SARU_CORE_ENCODE_H

#include "rust-url/rust-url.h"
#include "saru/core.h"

namespace saru {

// TODO(performance): introduce a version that writes into an existing buffer, and use that
// with the hostcall buffer where possible.
// https://github.com/fastly/js-compute-runtime/issues/215
saru::String encode(JSContext *cx, JS::HandleString str);
saru::String encode(JSContext *cx, JS::HandleValue val);

jsurl::SpecString encode_spec_string(JSContext *cx, JS::HandleValue val);

} // namespace saru

#endif
