#ifndef SARU_RUNTIME_DEFINITIONS_H
#define SARU_RUNTIME_DEFINITIONS_H

#include "saru/builtin.h"
#include "saru/builtins/console.h"
#include "saru/core.h"

//
// SARU - Japanese Snow Monkey
// Spidermonkey WinterCG Bindings with configuration Host Interface
//
// The definitions in this file define the necessary unbound symbol
// imports which should be defined to implement the runtime.
//

using saru::Console;
using saru::AsyncHandle;
using saru::HostError;

// Console log handler
void saru_impl_console_log(Console::LogType log_ty, const char *msg);

/// Generate an error in the JSContext.
void saru_handle_error(JSContext *cx, saru::Error err, int line, const char *func);

/// Async handlers
bool saru_async_io_is_ready(uint32_t handle, bool *ret, uint8_t *err);

bool saru_async_io_select(const AsyncHandle *handles, size_t handle_cnt, uint32_t timeout_ms, uint32_t *maybe_ret, HostError *err);

bool saru_http_body_new(uint32_t *handle, uint8_t *err);

bool saru_http_body_read(uint32_t handle, uint32_t chunk_size, char **ret, size_t *ret_cnt, HostError *err);

#define SARU_HTTP_BODY_WRITE_END_FRONT 0
#define SARU_HTTP_BODY_WRITE_END_BACK 1

bool saru_http_body_write(uint32_t handle, const uint8_t *chunk, size_t chunk_len, uint8_t write_end, uint32_t *written, HostError *err);

bool saru_http_body_append(uint32_t handle, uint32_t other_handle, HostError *err);

bool saru_http_body_close(uint32_t handle, HostError *err);

bool saru_http_body_close(uint32_t handle, uint8_t *err);

#endif
