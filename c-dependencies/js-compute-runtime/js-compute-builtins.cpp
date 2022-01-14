#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>

#include "../xqd.h"
#include "js-compute-builtins.h"

#include "rust-url/rust-url.h"

#include "js/Array.h"
#include "js/ArrayBuffer.h"
#include "js/Conversions.h"

// TODO: remove these once the warnings are fixed
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#include "js/experimental/TypedData.h"
#pragma clang diagnostic pop

#include "js/friend/DumpFunctions.h"
#include "js/JSON.h"
#include "js/shadow/Object.h"
#include "js/Stream.h"
#include "js/experimental/TypedData.h"
#include "js/Value.h"

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::UniqueChars;

using JS::Value;
using JS::ObjectValue;
using JS::ObjectOrNullValue;
using JS::PrivateValue;

using JS::RootedValue;
using JS::RootedObject;
using JS::RootedString;

using JS::HandleValue;
using JS::HandleValueArray;
using JS::HandleObject;
using JS::HandleString;
using JS::MutableHandleValue;

using JS::PersistentRooted;

template<auto fun>
bool internal_method(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject self(cx, &js::GetFunctionNativeReserved(&args.callee(), 0).toObject());
    return fun(cx, self, args);
}

template<auto fun>
JSObject* create_internal_method(JSContext* cx, HandleObject receiver, unsigned int nargs = 0,
                                 const char* name = "")
{
  JSFunction* method = js::NewFunctionWithReserved(cx, internal_method<fun>, 1, 0, name);
  if (!method) return nullptr;
  RootedObject method_obj(cx, JS_GetFunctionObject(method));
  js::SetFunctionNativeReserved(method_obj, 0, JS::ObjectValue(*receiver));
  return method_obj;
}

// Ensure that all the things we want to use the hostcall buffer for actually fit into the buffer.
#define HOSTCALL_BUFFER_LEN DICTIONARY_ENTRY_MAX_LEN
static_assert(HEADER_MAX_LEN < HOSTCALL_BUFFER_LEN);
static_assert(METHOD_MAX_LEN < HOSTCALL_BUFFER_LEN);
static_assert(URI_MAX_LEN < HOSTCALL_BUFFER_LEN);


class OwnedHostCallBuffer {
  static char* hostcall_buffer;
  char* borrowed_buffer;

public:
  static bool initialize(JSContext* cx) {
    // Ensure the buffer is all zeros so it doesn't add too much to the snapshot.
    hostcall_buffer = (char*)js_calloc(HOSTCALL_BUFFER_LEN);
    return !!hostcall_buffer;
  }

  OwnedHostCallBuffer() {
    MOZ_RELEASE_ASSERT(hostcall_buffer != nullptr);
    borrowed_buffer = hostcall_buffer;
    hostcall_buffer = nullptr;
  }

  char* get() {
    return borrowed_buffer;
  }

  ~OwnedHostCallBuffer() {
    // TODO: consider adding a build config that makes this zero the buffer.
    hostcall_buffer = borrowed_buffer;
  }
};

char* OwnedHostCallBuffer::hostcall_buffer;

using jsurl::SpecSlice, jsurl::SpecString, jsurl::JSUrl, jsurl::JSUrlSearchParams, jsurl::JSSearchParam;

static JS::PersistentRootedObjectVector* pending_requests;
static JS::PersistentRootedObjectVector* pending_body_reads;

// TODO: introduce a version that writes into an existing buffer, and use that
// with the hostcall buffer where possible.
UniqueChars encode(JSContext* cx, HandleString str, size_t* encoded_len) {
  UniqueChars text = JS_EncodeStringToUTF8(cx, str);
  if (!text)
    return nullptr;

  // This shouldn't fail, since the encode operation ensured `str` is linear.
  JSLinearString* linear = JS_EnsureLinearString(cx, str);
  *encoded_len = JS::GetDeflatedUTF8StringLength(linear);
  return text;
}

UniqueChars encode(JSContext* cx, HandleValue val, size_t* encoded_len) {
  RootedString str(cx, JS::ToString(cx, val));
  if (!str) return nullptr;

  return encode(cx, str, encoded_len);
}

SpecString encode(JSContext* cx, HandleValue val) {
  SpecString slice(nullptr, 0, 0);
  auto chars = encode(cx, val, &slice.len);
  if (!chars) return slice;
  slice.data = (uint8_t*)chars.release();
  slice.cap = slice.len;
  return slice;
}

/* Returns false if an exception is set on `cx` and the caller should immediately
   return to propagate the exception. */
static inline bool handle_fastly_result(JSContext* cx, int result, int line, const char* func) {
    switch(result)
    {
        case 0:
            return true;
        case 1:
            JS_ReportErrorUTF8(cx, "%s: Generic error value. This means that some unexpected error occurred during a hostcall. - Fastly error code %d\n", func, result);
            return false;
        case 2:
             JS_ReportErrorUTF8(cx, "%s: Invalid argument. - Fastly error code %d\n", func, result);
            return false;
        case 3:
             JS_ReportErrorUTF8(cx, "%s: Invalid handle. Thrown when a request, response, dictionary, or body handle is not valid. - Fastly error code %d\n", func, result);
            return false;
        case 4:
             JS_ReportErrorUTF8(cx, "%s: Buffer length error. Buffer is too long. - Fastly error code %d\n", func, result);
            return false;
        case 5:
             JS_ReportErrorUTF8(cx, "%s: Unsupported operation error. This error is thrown when some operation cannot be performed, because it is not supported. - Fastly error code %d\n", func, result);
            return false;
        case 6:
             JS_ReportErrorUTF8(cx, "%s: Alignment error. This is thrown when a pointer does not point to a properly aligned slice of memory. - Fastly error code %d\n", func, result);
            return false;
        case 7:
             JS_ReportErrorUTF8(cx, "%s: HTTP parse error. This can be thrown when a method, URI, header, or status is not valid. This can also be thrown if a message head is too large. - Fastly error code %d\n", func, result);
            return false;
        case 8:
             JS_ReportErrorUTF8(cx, "%s: HTTP user error. This is thrown in cases where user code caused an HTTP error. For example, attempt to send a 1xx response code, or a request with a non-absolute URI. This can also be caused by an unexpected header: both `content-length` and `transfer-encoding`, for example. - Fastly error code %d\n", func, result);
            return false;
        case 9:
             JS_ReportErrorUTF8(cx, "%s: HTTP incomplete message error. A stream ended unexpectedly. - Fastly error code %d\n", func, result);
            return false;
        case 10:
             JS_ReportErrorUTF8(cx, "%s: A `None` error. This status code is used to indicate when an optional value did not exist, as opposed to an empty value. - Fastly error code %d\n", func, result);
            return false;
        case 11:
             JS_ReportErrorUTF8(cx, "%s: HTTP head too large error. This error will be thrown when the message head is too large. - Fastly error code %d\n", func, result);
            return false;
        case 12:
             JS_ReportErrorUTF8(cx, "%s: HTTP invalid status error. This error will be thrown when the HTTP message contains an invalid status code. - Fastly error code %d\n", func, result);
             return false;
        default:
            fprintf(stdout, __FILE__":%d (%s) - Fastly error code %d\n", line, func, result);
            JS_ReportErrorUTF8(cx, "Fastly error code %d", result);
            return false;
    }
}

#define HANDLE_RESULT(cx, result) \
  handle_fastly_result(cx, result, __LINE__, __func__)

#define DBG(...) \
  printf("%s#%d: ", __func__, __LINE__); printf(__VA_ARGS__); fflush(stdout);

JSObject* PromiseRejectedWithPendingError(JSContext* cx) {
  RootedValue exn(cx);
  if (!JS_IsExceptionPending(cx) || !JS_GetPendingException(cx, &exn)) {
    return nullptr;
  }
  JS_ClearPendingException(cx);
  return JS::CallOriginalPromiseReject(cx, exn);
}

inline bool ReturnPromiseRejectedWithPendingError(JSContext* cx, const JS::CallArgs& args) {
  JSObject* promise = PromiseRejectedWithPendingError(cx);
  if (!promise) {
    return false;
  }

  args.rval().setObject(*promise);
  return true;
}

#define HANDLE_READ_CHUNK_SIZE 1024

template<auto op, class HandleType>
static char* read_from_handle_all(JSContext* cx, HandleType handle,
                                  size_t* nwritten, bool read_until_zero)
{
  // TODO: investigate passing a size hint in situations where we might know
  // the final size, e.g. via the `content-length` header.
  size_t buf_size = HANDLE_READ_CHUNK_SIZE;
  // TODO: make use of malloc slack.
  char* buf = static_cast<char*>(JS_malloc(cx, buf_size));
  if (!buf) {
      return nullptr;
  }

  // For realloc below.
  char* new_buf;

  size_t offset = 0;
  while (true) {
      size_t num_written = 0;
      int result = op(handle, buf + offset, HANDLE_READ_CHUNK_SIZE, &num_written);
      if (!HANDLE_RESULT(cx, result)) {
          JS_free(cx, buf);
          return nullptr;
      }

      offset += num_written;
      if (num_written == 0 ||
          (!read_until_zero && num_written < HANDLE_READ_CHUNK_SIZE))
      {
          break;
      }

      // TODO: make use of malloc slack, and use a smarter buffer growth strategy.
      size_t new_size = buf_size + HANDLE_READ_CHUNK_SIZE;
      new_buf = static_cast<char*>(JS_realloc(cx, buf, buf_size, new_size));
      if (!new_buf) {
        JS_free(cx, buf);
        return nullptr;
      }
      buf = new_buf;

      buf_size += HANDLE_READ_CHUNK_SIZE;
  }

  new_buf = static_cast<char*>(JS_realloc(cx, buf, buf_size, offset + 1));
  if (!buf) {
    JS_free(cx, buf);
    return nullptr;
  }
  buf = new_buf;

  buf[offset] = '\0';
  *nwritten = offset;

  return buf;
}

#define MULTI_VALUE_HOSTCALL(op, accum) \
    uint32_t cursor = 0; \
    int64_t ending_cursor = 0; \
    size_t nwritten; \
 \
    while (true) { \
        op \
 \
        if (nwritten == 0) { \
            break; \
        } \
 \
        accum \
 \
        if (ending_cursor < 0) { \
            break; \
        } \
 \
        cursor = (uint32_t)ending_cursor; \
    }

static constexpr const JSClassOps class_ops = {};
static const uint32_t class_flags = 0;

#define CLASS_BOILERPLATE_CUSTOM_INIT(cls) \
  const JSClass class_ = { #cls, JSCLASS_HAS_RESERVED_SLOTS(Slots::Count) | class_flags, \
                           &class_ops }; \
  static PersistentRooted<JSObject*> proto_obj; \
 \
  bool is_instance(JSObject* obj) { \
    return !!obj && JS::GetClass(obj) == &class_; \
  } \
 \
  bool is_instance(JS::Value val) { \
    return val.isObject() && is_instance(&val.toObject()); \
  } \
 \
  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name) { \
    if (!is_instance(receiver)) { \
      JS_ReportErrorUTF8(cx, "Method %s called on receiver that's not an instance of %s\n", \
                         method_name, class_.name); \
      return false; \
    } \
    return true; \
  }; \
 \
  bool init_class_impl(JSContext* cx, HandleObject global, \
                                    HandleObject parent_proto = nullptr) \
  { \
    proto_obj.init(cx, JS_InitClass(cx, global, parent_proto, &class_, constructor, ctor_length, \
                                    properties, methods, nullptr, nullptr)); \
    return proto_obj; \
  }; \

#define CLASS_BOILERPLATE(cls) \
  CLASS_BOILERPLATE_CUSTOM_INIT(cls) \
 \
  bool init_class(JSContext* cx, HandleObject global) { \
    return init_class_impl(cx, global); \
  } \

#define CLASS_BOILERPLATE_NO_CTOR(cls) \
  bool constructor(JSContext* cx, unsigned argc, Value* vp) { \
    JS_ReportErrorUTF8(cx, #cls " can't be instantiated directly"); \
    return false; \
  } \
 \
  CLASS_BOILERPLATE_CUSTOM_INIT(cls) \
 \
  bool init_class(JSContext* cx, HandleObject global) { \
    /* Right now, deleting the ctor from the global object after class \
       initialization seems to be the best we can do. Not ideal, but works. */ \
    return init_class_impl(cx, global) && \
           JS_DeleteProperty(cx, global, class_.name); \
  } \

// Define this to make most methods print their name to stderr when invoked.
// #define TRACE_METHOD_CALLS

#ifdef TRACE_METHOD_CALLS
#define TRACE_METHOD(name) \
  DBG("%s\n", name)
#else
#define TRACE_METHOD(name)
#endif

#define METHOD_HEADER_WITH_NAME(required_argc, name) \
  TRACE_METHOD(name) \
  CallArgs args = CallArgsFromVp(argc, vp); \
  if (!check_receiver(cx, args.thisv(), name)) return false; \
  RootedObject self(cx, &args.thisv().toObject()); \
  if (!args.requireAtLeast(cx, name, required_argc)) \
    return false; \

#define METHOD_HEADER(required_argc) \
  METHOD_HEADER_WITH_NAME(required_argc, __func__)

#define REQUEST_HANDLER_ONLY(name) \
  if (!FetchEvent::instance()) { \
    JS_ReportErrorUTF8(cx, "%s can only be used during request handling, " \
                           "not during initialization", name); \
    return false; \
  }

#define INIT_ONLY(name) \
  if (FetchEvent::instance()) { \
    JS_ReportErrorUTF8(cx, "%s can only be used during initialization, " \
                           "not during request handling", name); \
    return false; \
  }

namespace Logger {
  namespace Slots { enum {
    Endpoint,
    Count
  };};

  const unsigned ctor_length = 1;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  static bool log(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    auto endpoint = LogEndpointHandle { (uint32_t)JS::GetReservedSlot(self, Slots::Endpoint).toInt32() };

    size_t msg_len;
    UniqueChars msg = encode(cx, args.get(0), &msg_len);
    if (!msg) return false;

    size_t nwritten;
    if (!HANDLE_RESULT(cx, xqd_log_write(endpoint, msg.get(), msg_len, &nwritten)))
      return false;

    args.rval().setUndefined();
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("log", log, 1, JSPROP_ENUMERATE),
    JS_FS_END
  };

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE_NO_CTOR(Logger)

  JSObject* create(JSContext* cx, const char* name) {
    RootedObject logger(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!logger) return nullptr;

    auto handle = LogEndpointHandle { INVALID_HANDLE };

    if (!HANDLE_RESULT(cx, xqd_log_endpoint_get(name, strlen(name), &handle)))
      return nullptr;

    JS::SetReservedSlot(logger, Slots::Endpoint, JS::Int32Value(handle.handle));

    return logger;
  }
}

namespace Env {
  bool env_get(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "fastly.env.get", 1))
      return false;

    size_t var_name_len;
    UniqueChars var_name_chars = encode(cx, args[0], &var_name_len);
    if (!var_name_chars) {
      return false;
    }
    RootedString env_var(cx, JS_NewStringCopyZ(cx, getenv(var_name_chars.get())));
    if (!env_var) return false;

    args.rval().setString(env_var);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("get", env_get, 1, 0),
  JS_FS_END};

  JSObject* create(JSContext* cx) {
    RootedObject env(cx, JS_NewPlainObject(cx));
    if (!env || !JS_DefineFunctions(cx, env, methods)) return nullptr;
    return env;
  }
}

namespace URL {
  bool is_instance(JS::Value);
  JSObject* create(JSContext* cx, SpecString url_str, const JSUrl* base = nullptr);
  JSObject* create(JSContext* cx, HandleValue url_val, HandleObject base_obj);
}

static JSString* get_geo_info(JSContext* cx, HandleString address_str);

namespace Fastly {

  static bool debug_logging_enabled = false;

  static PersistentRooted<JSObject*> env;

  static PersistentRooted<JSObject*> baseURL;
  static PersistentRooted<JSString*> defaultBackend;

  bool dump(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, __func__, 1))
      return false;

    dump_value(cx, args[0], stdout);

    args.rval().setUndefined();
    return true;
  }

  bool enableDebugLogging(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, __func__, 1))
      return false;

    debug_logging_enabled = JS::ToBoolean(args[0]);

    args.rval().setUndefined();
    return true;
  }

  bool getGeolocationForIpAddress(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    REQUEST_HANDLER_ONLY("fastly.getGeolocationForIpAddress");
    if (!args.requireAtLeast(cx, "fastly.getGeolocationForIpAddress", 1))
      return false;

    RootedString address_str(cx, JS::ToString(cx, args[0]));
    if (!address_str)
      return false;

    RootedString geo_info_str(cx, get_geo_info(cx, address_str));
    if (!geo_info_str) return false;

    return JS_ParseJSON(cx, geo_info_str, args.rval());
  }

  // TODO: consider allowing logger creation during initialization, but then throw when trying
  // to log.
  bool getLogger(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    REQUEST_HANDLER_ONLY("fastly.getLogger");
    RootedObject self(cx, &args.thisv().toObject());
    if (!args.requireAtLeast(cx, "fastly.getLogger", 1))
      return false;

    size_t name_len;
    UniqueChars name = encode(cx, args[0], &name_len);
    if (!name) return false;

    RootedObject logger(cx, Logger::create(cx, name.get()));
    if (!logger) return false;

    args.rval().setObject(*logger);
    return true;
  }

  bool includeBytes(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    INIT_ONLY("fastly.includeBytes");
    RootedObject self(cx, &args.thisv().toObject());
    if (!args.requireAtLeast(cx, "fastly.includeBytes", 1))
      return false;

    size_t path_len;
    UniqueChars path = encode(cx, args[0], &path_len);
    if (!path) return false;

    FILE* fp = fopen(path.get(), "r");
    if (!fp) {
      JS_ReportErrorUTF8(cx, "Error opening file %s", path.get());
      return false;
    }

    fseek(fp, 0L, SEEK_END);
    size_t size = ftell(fp);
    rewind(fp);
    RootedObject typed_array(cx, JS_NewUint8Array(cx, size));
    if (!typed_array) return false;

    size_t read_bytes;
    {
      JS::AutoCheckCannotGC noGC(cx);
      bool is_shared;
      void* buffer = JS_GetArrayBufferViewData(typed_array, &is_shared, noGC);
      read_bytes = fread(buffer, 1, size, fp);
    }

    if (read_bytes != size) {
      JS_ReportErrorUTF8(cx, "Failed to read contents of file %s", path.get());
      return false;
    }

    args.rval().setObject(*typed_array);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("dump", dump, 1, 0),
    JS_FN("enableDebugLogging", enableDebugLogging, 1, JSPROP_ENUMERATE),
    JS_FN("getGeolocationForIpAddress", getGeolocationForIpAddress, 1, JSPROP_ENUMERATE),
    JS_FN("getLogger", getLogger, 1, JSPROP_ENUMERATE),
    JS_FN("includeBytes", includeBytes, 1, JSPROP_ENUMERATE),
    JS_FS_END
  };

  bool env_get(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setObject(*env);
    return true;
  }

  bool baseURL_get(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setObjectOrNull(baseURL);
    return true;
  }

  bool baseURL_set(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.get(0).isNullOrUndefined()) {
      baseURL.set(nullptr);
    }
    else if (!URL::is_instance(args.get(0))) {
      JS_ReportErrorUTF8(cx,
                         "Invalid value assigned to fastly.baseURL, must be an instance of "
                         "URL, null, or undefined");
      return false;
    }

    baseURL.set(&args.get(0).toObject());

    args.rval().setObjectOrNull(baseURL);
    return true;
  }

  bool defaultBackend_get(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(defaultBackend);
    return true;
  }

  bool defaultBackend_set(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString backend(cx, JS::ToString(cx, args.get(0)));
    if (!backend) return false;

    defaultBackend = backend;
    args.rval().setUndefined();
    return true;
  }

  const JSPropertySpec properties[] = {
    JS_PSG("env", env_get, JSPROP_ENUMERATE),
    JS_PSGS("baseURL", baseURL_get, baseURL_set, JSPROP_ENUMERATE),
    JS_PSGS("defaultBackend", defaultBackend_get, defaultBackend_set, JSPROP_ENUMERATE),
  JS_PS_END};

  static bool create(JSContext* cx, HandleObject global) {
    RootedObject fastly(cx, JS_NewPlainObject(cx));
    if (!fastly) return false;

    env.init(cx, Env::create(cx));
    if (!env) return false;
    baseURL.init(cx);
    defaultBackend.init(cx);

    if (!JS_DefineProperty(cx, global, "fastly", fastly, 0)) return false;
    return JS_DefineFunctions(cx, fastly, methods) &&
           JS_DefineProperties(cx, fastly, properties);
  }
}

namespace Console {
  template<const char* prefix, uint8_t prefix_len>
  static bool console_out(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    size_t msg_len;
    UniqueChars msg = encode(cx, args.get(0), &msg_len);
    if (!msg) return false;

    printf("%s: %s\n", prefix, msg.get());
    fflush(stdout);

    args.rval().setUndefined();
    return true;
  }

  static constexpr char PREFIX_LOG[] = "Log";
  static constexpr char PREFIX_TRACE[] = "Trace";
  static constexpr char PREFIX_INFO[] = "Info";
  static constexpr char PREFIX_WARN[] = "Warn";
  static constexpr char PREFIX_ERROR[] = "Error";

  const JSFunctionSpec methods[] = {
    JS_FN("log", (console_out<PREFIX_LOG, 3>), 1, JSPROP_ENUMERATE),
    JS_FN("trace", (console_out<PREFIX_TRACE, 5>), 1, JSPROP_ENUMERATE),
    JS_FN("info", (console_out<PREFIX_INFO, 4>), 1, JSPROP_ENUMERATE),
    JS_FN("warn", (console_out<PREFIX_WARN, 4>), 1, JSPROP_ENUMERATE),
    JS_FN("error", (console_out<PREFIX_ERROR, 5>), 1, JSPROP_ENUMERATE),
    JS_FS_END
  };

  static bool create(JSContext* cx, HandleObject global) {
    RootedObject console(cx, JS_NewPlainObject(cx));
    if (!console) return false;
    if (!JS_DefineProperty(cx, global, "console", console, JSPROP_ENUMERATE)) return false;
    return JS_DefineFunctions(cx, console, methods);
  }
}

bool is_int_typed_array(JSObject* obj) {
  return JS_IsInt8Array(obj) ||
         JS_IsUint8Array(obj) ||
         JS_IsInt16Array(obj) ||
         JS_IsUint16Array(obj) ||
         JS_IsInt32Array(obj) ||
         JS_IsUint32Array(obj) ||
         JS_IsUint8ClampedArray(obj);
}

namespace Crypto {

  #define MAX_BYTE_LENGTH 65536

  /**
   * Implementation of https://www.w3.org/TR/WebCryptoAPI/#Crypto-method-getRandomValues
   * TODO: investigate ways to automatically wipe the buffer passed in here when it is
   * GC'd. Content can roughly approximate that using finalizers for views of the buffer,
   * but it's far from ideal.
   */
  bool get_random_values(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "crypto.getRandomValues", 1))
      return false;

    if (!args[0].isObject() || !is_int_typed_array(&args[0].toObject())) {
      JS_ReportErrorUTF8(cx, "crypto.getRandomValues: input must be an integer-typed TypedArray");
      return false;
    }

    RootedObject typed_array(cx, &args[0].toObject());
    size_t byte_length = JS_GetArrayBufferViewByteLength(typed_array);
    if (byte_length > MAX_BYTE_LENGTH) {
      JS_ReportErrorUTF8(cx, "crypto.getRandomValues: input byteLength must be at most %u, "
                              "but is %zu", MAX_BYTE_LENGTH, byte_length);
        return false;
    }

    JS::AutoCheckCannotGC noGC(cx);
    bool is_shared;
    void* buffer = JS_GetArrayBufferViewData(typed_array, &is_shared, noGC);
    arc4random_buf(buffer, byte_length);

    args.rval().setObject(*typed_array);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("getRandomValues", get_random_values, 1, JSPROP_ENUMERATE),
    JS_FS_END
  };

  static bool create(JSContext* cx, HandleObject global) {
    RootedObject crypto(cx, JS_NewPlainObject(cx));
    if (!crypto) return false;
    if (!JS_DefineProperty(cx, global, "crypto", crypto, JSPROP_ENUMERATE)) return false;
    return JS_DefineFunctions(cx, crypto, methods);
  }
}

namespace NativeStreamSource {
  typedef bool PullAlgorithm(JSContext* cx, CallArgs args, HandleObject stream, HandleObject owner,
                             HandleObject controller);
  typedef bool CancelAlgorithm(JSContext* cx, CallArgs args, HandleObject stream,
                               HandleObject owner, HandleValue reason);

  JSObject* create(JSContext* cx, HandleObject owner, HandleValue startPromise,
                   PullAlgorithm* pull, CancelAlgorithm* cancel);
  static JSObject* get_stream_source(JSContext* cx, HandleObject stream);
  JSObject* owner(JSObject* self);
  JSObject* stream(JSObject* self);
  bool stream_has_native_source(JSContext* cx, HandleObject stream);
  bool stream_is_body(JSContext* cx, HandleObject stream);
  bool lock_stream(JSContext* cx, HandleObject stream);
  JSObject* piped_to_transform_stream(JSObject* source);
}

enum class BodyReadResult {
    ArrayBuffer,
    JSON,
    Text,
};

namespace Headers {
  enum class Mode : int32_t {
    Standalone,
    ProxyToRequest,
    ProxyToResponse
  };

  JSObject* create(JSContext* cx, Mode mode, HandleObject owner);
  JSObject* create(JSContext* cx, Mode mode, HandleObject owner, HandleValue initv);

  bool delazify(JSContext* cx, HandleObject headers);
}

namespace Request {
  bool is_instance(JSObject* obj);
}

namespace Response {
  bool is_instance(JSObject* obj);
}

namespace TransformStream {
  namespace Slots { enum {
    Controller,
    Readable,
    Writable,
    Backpressure,
    BackpressureChangePromise,
    Owner, // The target RequestOrResponse object if the stream's readable end is used as a body.
    Count
  };};

  bool is_instance(JSObject* obj);

  JSObject* owner(JSObject* self);
  void set_owner(JSObject* self, JSObject* owner);
  JSObject* readable(JSObject* self);
  bool is_ts_readable(JSContext* cx, HandleObject readable);
  bool readable_used_as_body(JSObject* self);
  void set_readable_used_as_body(JSContext* cx, HandleObject readable, HandleObject target);
  bool is_ts_writable(JSContext* cx, HandleObject writable);
  JSObject* controller(JSObject* self);
  bool backpressure(JSObject* self);

  bool ErrorWritableAndUnblockWrite(JSContext* cx, HandleObject stream, HandleValue error);
  bool SetBackpressure(JSContext* cx, HandleObject stream, bool backpressure);
  bool Error(JSContext* cx, HandleObject stream, HandleValue error);
}

namespace RequestOrResponse {
  namespace Slots { enum {
    RequestOrResponse,
    Body,
    BodyStream,
    HasBody,
    BodyUsed,
    Headers,
    URL,
    Count
  };};

  bool is_instance(JSObject* obj) {
    return Request::is_instance(obj) || Response::is_instance(obj);
  }

  uint32_t handle(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return static_cast<uint32_t>(JS::GetReservedSlot(obj, Slots::RequestOrResponse).toInt32());
  }

  bool has_body(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return JS::GetReservedSlot(obj, Slots::HasBody).toBoolean();
  }

  BodyHandle body_handle(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return BodyHandle { static_cast<uint32_t>(JS::GetReservedSlot(obj, Slots::Body).toInt32()) };
  }

  JSObject* body_stream(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    // Can't use toObjectOrNull here because the Value might be `undefined`.
    Value val = JS::GetReservedSlot(obj, Slots::BodyStream);
    return val.isNullOrUndefined() ? nullptr : &val.toObject();
  }

  JSObject* body_source(JSContext* cx, HandleObject obj) {
    MOZ_ASSERT(has_body(obj));
    RootedObject stream(cx, body_stream(obj));
    return NativeStreamSource::get_stream_source(cx, stream);
  }

  bool mark_body_used(JSContext* cx, HandleObject obj) {
    MOZ_ASSERT(is_instance(obj));
    JS::SetReservedSlot(obj, Slots::BodyUsed, JS::BooleanValue(true));

    RootedObject stream(cx, body_stream(obj));
    if (stream)
      return NativeStreamSource::lock_stream(cx, stream);

    return true;
  }

  /**
   * Moves an underlying body handle from one Request/Response object to another.
   *
   * Also marks the source object's body as consumed.
   */
  bool move_body_handle(JSContext* cx, HandleObject from, HandleObject to) {
    MOZ_ASSERT(is_instance(from));
    MOZ_ASSERT(is_instance(to));


    // Replace the receiving object's body handle with the body stream source's underlying handle.
    // TODO: Let the host know we'll not use the old handle anymore, once C@E has a hostcall
    // for that.
    BodyHandle body = body_handle(from);
    JS::SetReservedSlot(to, Slots::Body, JS::Int32Value(body.handle));

    // Mark the source's body as used, and the stream as locked to prevent any future attempts
    // to use the underlying handle we just removed.
    return mark_body_used(cx, from);
  }

  Value url(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    Value val = JS::GetReservedSlot(obj, Slots::URL);
    MOZ_ASSERT(val.isString());
    return val;
  }

  void set_url(JSObject* obj, Value url) {
    MOZ_ASSERT(is_instance(obj));
    MOZ_ASSERT(url.isString());
    JS::SetReservedSlot(obj, Slots::URL, url);
  }

  bool body_used(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return JS::GetReservedSlot(obj, Slots::BodyUsed).toBoolean();
  }

  bool set_body(JSContext*cx, HandleObject obj, HandleValue body_val) {
    MOZ_ASSERT(is_instance(obj));
    if (body_val.isNullOrUndefined()) {
      JS::SetReservedSlot(obj, Slots::HasBody, JS::BooleanValue(false));
      return true;
    }

    // TODO: throw if method is GET or HEAD.

    // TODO: properly implement the spec steps for extracting the body and setting the content-type.
    //

    // TODO: Support the other possible inputs to Body.

    RootedObject body_obj(cx, body_val.isObject() ? &body_val.toObject() : nullptr);

    if (body_obj && JS::IsReadableStream(body_obj)) {
      JS_SetReservedSlot(obj, RequestOrResponse::Slots::BodyStream, body_val);
      if (TransformStream::is_ts_readable(cx, body_obj)) {
        TransformStream::set_readable_used_as_body(cx, body_obj, obj);
      }
    } else {
      mozilla::Maybe<JS::AutoCheckCannotGC> maybeNoGC;
      UniqueChars text;
      char* buf;
      size_t length;

      if (body_obj && JS_IsArrayBufferViewObject(body_obj)) {
        // Short typed arrays have inline data which can move on GC, so assert that no GC happens.
        // (Which it doesn't, because we're not allocating before `buf` goes out of scope.)
        maybeNoGC.emplace(cx);
        JS::AutoCheckCannotGC& noGC = maybeNoGC.ref();
        bool is_shared;
        length = JS_GetArrayBufferViewByteLength(body_obj);
        buf = (char*)JS_GetArrayBufferViewData(body_obj, &is_shared, noGC);
      } else if (body_obj && JS::IsArrayBufferObject(body_obj)) {
        bool is_shared;
        JS::GetArrayBufferLengthAndData(body_obj, &length, &is_shared, (uint8_t**)&buf);
      } else {
        text = encode(cx, body_val, &length);
        if (!text) return false;
        buf = text.get();
      }

      BodyHandle body_handle = RequestOrResponse::body_handle(obj);
      size_t num_written = 0;
      int result = xqd_body_write(body_handle, buf, length, BodyWriteEndBack, &num_written);
      if (!HANDLE_RESULT(cx, result))
        return false;
    }

    JS::SetReservedSlot(obj, Slots::HasBody, JS::BooleanValue(true));
    return true;
  }

  template<auto mode>
  JSObject* headers(JSContext*cx, HandleObject obj) {
    Value val = JS::GetReservedSlot(obj, Slots::Headers);
    if (val.isNullOrUndefined()) {
      JSObject* headers = Headers::create(cx, mode, obj);
      if (!headers) return nullptr;
      val = JS::ObjectValue(*headers);
      JS_SetReservedSlot(obj, Slots::Headers, val);
    }
    return &val.toObject();
  }

  bool append_body(JSContext* cx, HandleObject self, HandleObject source) {
    BodyHandle source_body = body_handle(source);
    BodyHandle dest_body = body_handle(self);
    return HANDLE_RESULT(cx, xqd_body_append(dest_body, source_body));
  }

  template<BodyReadResult result_type>
  bool bodyAll(JSContext* cx, CallArgs args, HandleObject self) {
    // TODO: mark body as consumed when operating on stream, too.
    if (body_used(self)) {
      JS_ReportErrorASCII(cx, "Body has already been consumed");
      return ReturnPromiseRejectedWithPendingError(cx, args);
    }

    BodyHandle body = body_handle(self);

    // TODO: check if this should be lazified. JS code might expect to be able to trigger
    // multiple requests for response body contents in parallel instead of blocking
    // on them sequentially.
    size_t bytes_read;
    UniqueChars buf(read_from_handle_all<xqd_body_read, BodyHandle>(cx, body, &bytes_read, true));
    if (!buf) {
      return ReturnPromiseRejectedWithPendingError(cx, args);
    }

    if (!mark_body_used(cx, self))
      return false;

    RootedValue result(cx);

    if (result_type == BodyReadResult::ArrayBuffer) {
      auto* rawBuf = buf.release();
      RootedObject array_buffer(cx, JS::NewArrayBufferWithContents(cx, bytes_read, rawBuf));
      if (!array_buffer) {
        JS_free(cx, rawBuf);
        return ReturnPromiseRejectedWithPendingError(cx, args);
      }
      result.setObject(*array_buffer);
    } else {
      RootedString text(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(buf.get(), bytes_read)));
      if (!text) {
        return ReturnPromiseRejectedWithPendingError(cx, args);
      }

      if (result_type == BodyReadResult::Text) {
        result.setString(text);
      } else if (result_type == BodyReadResult::JSON) {
          if (!JS_ParseJSON(cx, text, &result)) {
            return ReturnPromiseRejectedWithPendingError(cx, args);
          }
      } else {
        MOZ_ASSERT_UNREACHABLE("Unsupported body read result type");
      }
    }

    RootedObject promise(cx, JS::CallOriginalPromiseResolve(cx, result));
    if (!promise) {
      return false;
    }

    args.rval().setObject(*promise);
    return true;
  }

  bool body_source_pull_algorithm(JSContext* cx, CallArgs args, HandleObject source,
                                  HandleObject body_owner, HandleObject controller)
  {
    // If the stream has been piped to a TransformStream whose readable end was then passed
    // to a Request or Response as the body, we can just append the entire source body to the
    // destination using a single native hostcall, and then close the source stream, instead of
    // reading and writing it in individual chunks.
    // Note that even in situations where multiple streams are piped to the same destination
    // this is guaranteed to happen in the right order: ReadableStream#pipeTo locks the destination
    // WritableStream until the source ReadableStream is closed/canceled, so only one stream can
    // ever be piped in at the same time.
    RootedObject pipe_dest(cx, NativeStreamSource::piped_to_transform_stream(source));
    if (pipe_dest) {
      if (TransformStream::readable_used_as_body(pipe_dest)) {
        RootedObject dest_owner(cx, TransformStream::owner(pipe_dest));
        if (!RequestOrResponse::append_body(cx, dest_owner, body_owner)) {
          return false;
        }

        RootedObject stream(cx, NativeStreamSource::stream(source));
        bool success = JS::ReadableStreamClose(cx, stream);
        MOZ_RELEASE_ASSERT(success);

        args.rval().setUndefined();
        return true;
      }
    }

    // The actual read from the body needs to be delayed, because it'd otherwise
    // be a blocking operation in case the backend didn't yet send any data.
    // That would lead to situations where we block on I/O before processing
    // all pending Promises, which in turn can result in operations happening in
    // observably different behavior, up to and including causing deadlocks
    // because a body read response is blocked on content making another request.
    //
    // (This deadlock happens in automated tests, but admittedly might not happen
    // in real usage.)

    if (!pending_body_reads->append(source))
      return false;

    args.rval().setUndefined();
    return true;
  }

  bool body_source_cancel_algorithm(JSContext* cx, CallArgs args, HandleObject stream,
                                    HandleObject owner, HandleValue reason)
  {
    args.rval().setUndefined();
    return true;
  }

  JSObject* create_body_stream(JSContext* cx, HandleObject owner) {
    RootedObject source(cx, NativeStreamSource::create(cx, owner, JS::UndefinedHandleValue,
                                                       body_source_pull_algorithm,
                                                       body_source_cancel_algorithm));
    if (!source) return nullptr;

    // Create a readable stream with a highwater mark of 0.0 to prevent an eager pull.
    // With the default HWM of 1.0, the streams implementation causes a pull, which means
    // we enqueue a read from the host handle, which we quite often have no interest in
    // at all.
    RootedObject body_stream(cx, JS::NewReadableDefaultStreamObject(cx, source, nullptr, 0.0));
    if (!body_stream) return nullptr;

    // This assert guarantees that we fail early in case SpiderMonkey ever changes how
    // the underlying source for streams is stored, instead of operating on invalid
    // values.
    MOZ_RELEASE_ASSERT(NativeStreamSource::get_stream_source(cx, body_stream) == source);

    JS_SetReservedSlot(owner, RequestOrResponse::Slots::BodyStream, JS::ObjectValue(*body_stream));
    return body_stream;
  }

  bool body_get(JSContext* cx, CallArgs args, HandleObject self, bool create_if_undefined) {
    if (!has_body(self)) {
      args.rval().setNull();
      return true;
    }

    RootedObject body_stream(cx, ::RequestOrResponse::body_stream(self));
    if (!body_stream && create_if_undefined) {
      body_stream = create_body_stream(cx, self);
      if (!body_stream) return false;
    }

    if (body_stream) {
      args.rval().setObject(*body_stream);
    }

    return true;
  }
}

namespace NativeStreamSink {
  static JSObject* get_stream_sink(JSContext* cx, HandleObject stream);
  JSObject* owner(JSObject* self);
}

// A JS class to use as the underlying source for native readable streams, used for
// Request/Response bodies and TransformStream.
namespace NativeStreamSource {
  namespace Slots { enum {
    Owner, // Request or Response object, or TransformStream.
    Controller, // The ReadableStreamDefaultController.
    InternalReader, // Only used to lock the stream if it's consumed internally.
    StartPromise, // Used as the return value of `start`, can be undefined.
                  // Needed to properly implement TransformStream.
    PullAlgorithm,
    CancelAlgorithm,
    PipedToTransformStream, // The TransformStream this source's stream is piped to, if any.
                            // Only applies if the source backs a RequestOrResponse's body.
    Count
  };};

  bool is_instance(JSObject* obj);

  JSObject* owner(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return &JS::GetReservedSlot(self, Slots::Owner).toObject();
  }

  JSObject* stream(JSObject* self) {
    return RequestOrResponse::body_stream(owner(self));
  }

  Value startPromise(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return JS::GetReservedSlot(self, Slots::StartPromise);
  }

  PullAlgorithm* pullAlgorithm(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return (PullAlgorithm*)JS::GetReservedSlot(self, Slots::PullAlgorithm).toPrivate();
  }

  CancelAlgorithm* cancelAlgorithm(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return (CancelAlgorithm*)JS::GetReservedSlot(self, Slots::CancelAlgorithm).toPrivate();
  }

  JSObject* controller(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return &JS::GetReservedSlot(self, Slots::Controller).toObject();
  }

  /**
   * Returns the underlying source for the given controller iff it's an object,
   * nullptr otherwise.
   */
  static JSObject* get_controller_source(JSContext* cx, HandleObject controller) {
    RootedValue source(cx);
    bool success __attribute__((unused));
    success = JS::ReadableStreamControllerGetUnderlyingSource(cx, controller, &source);
    MOZ_ASSERT(success);
    return source.isObject() ? &source.toObject() : nullptr;
  }

  static JSObject* get_stream_source(JSContext* cx, HandleObject stream) {
    MOZ_ASSERT(JS::IsReadableStream(stream));
    RootedObject controller(cx, JS::ReadableStreamGetController(cx, stream));
    return get_controller_source(cx, controller);
  }

  bool stream_has_native_source(JSContext* cx, HandleObject stream) {
    JSObject* source = get_stream_source(cx, stream);
    return is_instance(source);
  }

  bool stream_is_body(JSContext* cx, HandleObject stream) {
    JSObject* stream_source = get_stream_source(cx, stream);
    return NativeStreamSource::is_instance(stream_source) &&
           RequestOrResponse::is_instance(owner(stream_source));
  }

  void set_stream_piped_to_ts_writable(JSContext* cx, HandleObject stream, HandleObject writable) {
    RootedObject source(cx, NativeStreamSource::get_stream_source(cx, stream));
    MOZ_ASSERT(is_instance(source));
    RootedObject sink(cx, NativeStreamSink::get_stream_sink(cx, writable));
    RootedObject transform_stream(cx, NativeStreamSink::owner(sink));
    MOZ_ASSERT(transform_stream);
    JS::SetReservedSlot(source, Slots::PipedToTransformStream, ObjectValue(*transform_stream));
  }

  JSObject* piped_to_transform_stream(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return JS::GetReservedSlot(self, Slots::PipedToTransformStream).toObjectOrNull();
  }

  bool lock_stream(JSContext* cx, HandleObject stream) {
    MOZ_ASSERT(JS::IsReadableStream(stream));

    bool locked;
    JS::ReadableStreamIsLocked(cx, stream, &locked);
    MOZ_ASSERT(!locked);

    RootedObject self(cx, get_stream_source(cx, stream));
    MOZ_ASSERT(is_instance(self));

    auto mode = JS::ReadableStreamReaderMode::Default;
    RootedObject reader(cx, JS::ReadableStreamGetReader(cx, stream, mode));
    if (!reader)
      return false;

    JS::SetReservedSlot(self, Slots::InternalReader, JS::ObjectValue(*reader));
    return true;
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool start(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    MOZ_ASSERT(args[0].isObject());
    RootedObject controller(cx, &args[0].toObject());
    MOZ_ASSERT(get_controller_source(cx, controller) == self);

    JS::SetReservedSlot(self, Slots::Controller, args[0]);

    // For TransformStream, StartAlgorithm returns the same Promise for both the readable
    // and writable stream. All other native initializations of ReadableStream have StartAlgorithm
    // return undefined. Instead of introducing both the StartAlgorithm as a pointer and
    // startPromise as a value, we just store the latter or undefined, and always return it.
    args.rval().set(startPromise(self));
    return true;
  }

  bool pull(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    RootedObject owner(cx, NativeStreamSource::owner(self));
    RootedObject controller(cx, &args[0].toObject());
    MOZ_ASSERT(controller == NativeStreamSource::controller(self));
    MOZ_ASSERT(get_controller_source(cx, controller) == self.get());

    PullAlgorithm* pull = pullAlgorithm(self);
    return pull(cx, args, self, owner, controller);
  }

  bool cancel(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedObject owner(cx, NativeStreamSource::owner(self));
    HandleValue reason(args.get(0));

    CancelAlgorithm* cancel = cancelAlgorithm(self);
    return cancel(cx, args, self, owner, reason);
  }

  const JSFunctionSpec methods[] = {
    JS_FN("start", start, 1, 0),
    JS_FN("pull", pull, 1, 0),
    JS_FN("cancel", cancel, 1, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE_NO_CTOR(NativeStreamSource)

  JSObject* create(JSContext* cx, HandleObject owner, HandleValue startPromise,
                   PullAlgorithm* pull, CancelAlgorithm* cancel)
  {
    RootedObject source(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!source) return nullptr;

    JS::SetReservedSlot(source, Slots::Owner, JS::ObjectValue(*owner));
    JS::SetReservedSlot(source, Slots::StartPromise, startPromise);
    JS::SetReservedSlot(source, Slots::PullAlgorithm, JS::PrivateValue((void*)pull));
    JS::SetReservedSlot(source, Slots::CancelAlgorithm, JS::PrivateValue((void*)cancel));
    JS::SetReservedSlot(source, Slots::PipedToTransformStream, JS::NullValue());
    return source;
  }
}

// A JS class to use as the underlying sink for native writable streams, used for
// TransformStream.
namespace NativeStreamSink {
  typedef bool WriteAlgorithm(JSContext* cx, CallArgs args, HandleObject stream,
                              HandleObject owner, HandleValue chunk);
  typedef bool AbortAlgorithm(JSContext* cx, CallArgs args, HandleObject stream,
                              HandleObject owner, HandleValue reason);
  typedef bool CloseAlgorithm(JSContext* cx, CallArgs args, HandleObject stream,
                              HandleObject owner);
  namespace Slots { enum {
    Owner, // TransformStream.
    Controller, // The WritableStreamDefaultController.
    InternalWriter, // Only used to lock the stream if it's consumed internally.
    StartPromise, // Used as the return value of `start`, can be undefined.
                  // Needed to properly implement TransformStream.
    WriteAlgorithm,
    AbortAlgorithm,
    CloseAlgorithm,
    // AbortAlgorithm, TODO: implement
    Count
  };};

  bool is_instance(JSObject* obj);

  JSObject* owner(JSObject* self) {
    return &JS::GetReservedSlot(self, Slots::Owner).toObject();
  }

  Value startPromise(JSObject* self) {
    return JS::GetReservedSlot(self, Slots::StartPromise);
  }

  WriteAlgorithm* writeAlgorithm(JSObject* self) {
    return (WriteAlgorithm*)JS::GetReservedSlot(self, Slots::WriteAlgorithm).toPrivate();
  }

  AbortAlgorithm* abortAlgorithm(JSObject* self) {
    return (AbortAlgorithm*)JS::GetReservedSlot(self, Slots::AbortAlgorithm).toPrivate();
  }

  CloseAlgorithm* closeAlgorithm(JSObject* self) {
    return (CloseAlgorithm*)JS::GetReservedSlot(self, Slots::CloseAlgorithm).toPrivate();
  }

  JSObject* controller(JSObject* self) {
    return &JS::GetReservedSlot(self, Slots::Controller).toObject();
  }

  /**
   * Returns the underlying sink for the given controller iff it's an object,
   * nullptr otherwise.
   */
  static JSObject* get_controller_sink(JSContext* cx, HandleObject controller) {
    RootedValue sink(cx, JS::WritableStreamControllerGetUnderlyingSink(cx, controller));
    return sink.isObject() ? &sink.toObject() : nullptr;
  }

  static JSObject* get_stream_sink(JSContext* cx, HandleObject stream) {
    RootedObject controller(cx, JS::WritableStreamGetController(cx, stream));
    return get_controller_sink(cx, controller);
  }

  bool stream_has_native_sink(JSContext* cx, HandleObject stream) {
    MOZ_RELEASE_ASSERT(JS::IsWritableStream(stream));

    JSObject* sink = get_stream_sink(cx, stream);
    return is_instance(sink);
  }

  bool lock_stream(JSContext* cx, HandleObject stream) {
    MOZ_ASSERT(JS::IsWritableStream(stream));
    MOZ_ASSERT(!JS::WritableStreamIsLocked(cx, stream));

    RootedObject self(cx, get_stream_sink(cx, stream));
    MOZ_ASSERT(is_instance(self));

    RootedObject writer(cx, JS::WritableStreamGetWriter(cx, stream));
    if (!writer)
      return false;

    JS::SetReservedSlot(self, Slots::InternalWriter, JS::ObjectValue(*writer));
    return true;
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool start(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    MOZ_ASSERT(args[0].isObject());
    RootedObject controller(cx, &args[0].toObject());
    MOZ_ASSERT(get_controller_sink(cx, controller) == self);

    JS::SetReservedSlot(self, Slots::Controller, args[0]);

    // For TransformStream, StartAlgorithm returns the same Promise for both the readable
    // and writable stream. All other native initializations of WritableStream have
    // StartAlgorithm return undefined.
    //
    // Instead of introducing both the StartAlgorithm as a pointer and startPromise as a
    // value, we just store the latter or undefined, and always return it.
    args.rval().set(startPromise(self));
    return true;
  }

  bool write(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    RootedObject owner(cx, NativeStreamSink::owner(self));
    HandleValue chunk(args[0]);

    WriteAlgorithm* write = writeAlgorithm(self);
    return write(cx, args, self, owner, chunk);
  }

  bool abort(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    RootedObject owner(cx, NativeStreamSink::owner(self));
    HandleValue reason(args[0]);

    AbortAlgorithm* abort = abortAlgorithm(self);
    return abort(cx, args, self, owner, reason);
  }

  bool close(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedObject owner(cx, NativeStreamSink::owner(self));

    CloseAlgorithm* close = closeAlgorithm(self);
    return close(cx, args, self, owner);
  }

  const JSFunctionSpec methods[] = {
    JS_FN("start", start, 1, 0),
    JS_FN("write", write, 2, 0),
    JS_FN("abort", abort, 2, 0),
    JS_FN("close", close, 1, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE_NO_CTOR(NativeStreamSink)

  JSObject* create(JSContext* cx, HandleObject owner, HandleValue startPromise,
                   WriteAlgorithm* write, CloseAlgorithm* close, AbortAlgorithm* abort)
  {
    RootedObject sink(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!sink) return nullptr;

    JS::SetReservedSlot(sink, Slots::Owner, JS::ObjectValue(*owner));
    JS::SetReservedSlot(sink, Slots::StartPromise, startPromise);
    JS::SetReservedSlot(sink, Slots::WriteAlgorithm, JS::PrivateValue((void*)write));
    JS::SetReservedSlot(sink, Slots::AbortAlgorithm, JS::PrivateValue((void*)abort));
    JS::SetReservedSlot(sink, Slots::CloseAlgorithm, JS::PrivateValue((void*)close));
    return sink;
  }
}

namespace ReadableStream_additions {
  static PersistentRooted<JSObject*> proto_obj;

  bool is_instance(JSObject* obj) {
    return JS::IsReadableStream(obj);
  }

  bool is_instance(JS::Value val) {
    return val.isObject() && is_instance(&val.toObject());
  }

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name) {
    if (!is_instance(receiver)) {
      JS_ReportErrorUTF8(cx,
                         "Method %s called on receiver that's not an instance of ReadableStream",
                         method_name);
      return false;
    }
    return true;
  };

  static PersistentRooted<Value> original_pipeTo;

  bool pipeTo(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    // If the receiver is backed by a native source and the destination is the writable end of a
    // TransformStream, set the TransformStream as the owner of the receiver's source.
    // This enables us to shortcut operations later on.
    RootedObject target(cx, args[0].isObject() ? &args[0].toObject() : nullptr);
    if (target && NativeStreamSource::stream_has_native_source(cx, self) &&
        JS::IsWritableStream(target) && TransformStream::is_ts_writable(cx, target))
    {
        NativeStreamSource::set_stream_piped_to_ts_writable(cx, self, target);
    }

    return JS::Call(cx, args.thisv(), original_pipeTo, HandleValueArray(args), args.rval());
  }

  bool pipeThrough(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    if (!args[0].isObject()) {
      JS_ReportErrorLatin1(cx, "First argument to pipeThrough must be an object");
      return false;
    }

    RootedObject transform(cx, &args[0].toObject());
    RootedObject readable(cx);
    RootedObject writable(cx);

    RootedValue val(cx);
    if (!JS_GetProperty(cx, transform, "readable", &val))
      return false;
    if (!val.isObject() || !JS::IsReadableStream(&val.toObject())) {
      JS_ReportErrorLatin1(cx, "First argument to pipeThrough must be an object with a "
                               "|readable| property that is an instance of ReadableStream");
      return false;
    }

    readable = &val.toObject();

    if (!JS_GetProperty(cx, transform, "writable", &val))
      return false;
    if (!val.isObject() || !JS::IsWritableStream(&val.toObject())) {
      JS_ReportErrorLatin1(cx, "First argument to pipeThrough must be an object with a "
                               "|writable| property that is an instance of WritableStream");
      return false;
    }
    writable = &val.toObject();

    // 1. If ! IsReadableStreamLocked(this) is true, throw a TypeError exception.
    bool locked;
    if (!JS::ReadableStreamIsLocked(cx, self, &locked)) {
      return false;
    }
    if (locked) {
      JS_ReportErrorLatin1(cx, "pipeThrough called on a ReadableStream that's already locked");
      return false;
    }

    // 2. If ! IsWritableStreamLocked(transform["writable"]) is true, throw a TypeError exception.
    if (JS::WritableStreamIsLocked(cx, writable)) {
      JS_ReportErrorLatin1(cx, "The writable end of the transform object passed to pipeThrough "
                               " passed to pipeThrough is already locked");
      return false;
    }

    // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
    // (implicit, see note in step 4.)

    // 4. Let promise be ! ReadableStreamPipeTo(this, transform["writable"], options
    // ["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    // Note: instead of extracting the prevent* flags above, we just pass the |options|
    // argument as-is. pipeTo will fail eagerly if it fails to extract the fields on
    // |options|, so while skipping the extraction above changes the order in which errors
    // are reported and the error messages a bit, it otherwise preserves semantics.
    // In particular, the errors aren't reported as rejected promises, as would be the case
    // for those reported in steps 1 and 2.
    JS::RootedValueArray<2> newArgs(cx);
    newArgs[0].setObject(*writable);
    newArgs[1].set(args.get(1));
    if (!JS::Call(cx, args.thisv(), original_pipeTo, newArgs, &val)) {
      return false;
    }

    RootedObject promise(cx, &val.toObject());
    MOZ_ASSERT(JS::IsPromiseObject(promise));

    // 5. Set promise.[[PromiseIsHandled]] to true.
    // JSAPI doesn't provide a straightforward way to do this, but we can just register
    // null-reactions in a way that achieves it.
    if (!JS::AddPromiseReactionsIgnoringUnhandledRejection(cx, promise, nullptr, nullptr)) {
      return false;
    }

    // 6. Return transform["readable"].
    args.rval().setObject(*readable);
    return true;
  }

  bool initialize_additions(JSContext* cx, HandleObject global) {
    RootedValue val(cx);
    if (!JS_GetProperty(cx, global, "ReadableStream", &val))
      return false;
    RootedObject readableStream_builtin(cx, &val.toObject());

    if (!JS_GetProperty(cx, readableStream_builtin, "prototype", &val)) {
      return false;
    }
    proto_obj.init(cx, &val.toObject());
    MOZ_ASSERT(proto_obj);

    original_pipeTo.init(cx);
    if (!JS_GetProperty(cx, proto_obj, "pipeTo", &original_pipeTo))
      return false;
    MOZ_ASSERT(JS::IsCallable(&original_pipeTo.toObject()));

    if (!JS_DefineFunction(cx, proto_obj, "pipeTo", pipeTo, 1, JSPROP_ENUMERATE)) {
      return false;
    }

    if (!JS_DefineFunction(cx, proto_obj, "pipeThrough", pipeThrough, 1, JSPROP_ENUMERATE)) {
      return false;
    }

    return true;
  }
}

/**
 * Implementation of the WHATWG TransformStream builtin.
 *
 * All algorithm names and steps refer to spec algorithms defined at
 * https://streams.spec.whatwg.org/#ts-default-controller-class
 */
namespace TransformStreamDefaultController {

typedef JSObject* TransformAlgorithm(JSContext* cx, HandleObject controller, HandleValue chunk);
typedef JSObject* FlushAlgorithm(JSContext* cx, HandleObject controller);

  namespace Slots { enum {
    Stream,
    Transformer,
    TransformAlgorithm,
    TransformInput, // JS::Value to be used by TransformAlgorithm, e.g. a JSFunction to call.
    FlushAlgorithm,
    FlushInput, // JS::Value to be used by FlushAlgorithm, e.g. a JSFunction to call.
    Count
  };};

  bool is_instance(JSObject* obj);

  JSObject* stream(JSObject* controller) {
    MOZ_ASSERT(is_instance(controller));
    return &JS::GetReservedSlot(controller, Slots::Stream).toObject();
  }

  TransformAlgorithm* transformAlgorithm(JSObject* controller) {
    MOZ_ASSERT(is_instance(controller));
    return (TransformAlgorithm*)JS::GetReservedSlot(controller, Slots::TransformAlgorithm)
                                .toPrivate();
  }

  FlushAlgorithm* flushAlgorithm(JSObject* controller) {
    MOZ_ASSERT(is_instance(controller));
    return (FlushAlgorithm*)JS::GetReservedSlot(controller, Slots::FlushAlgorithm).toPrivate();
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool Enqueue(JSContext* cx, HandleObject controller, HandleValue chunk);
  bool Terminate(JSContext* cx, HandleObject controller);

  /**
   * https://streams.spec.whatwg.org/#ts-default-controller-desired-size
   */
  bool desiredSize_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(0, "get desiredSize")

    // 1.  Let readableController be [this].[stream].[readable].[controller].
    JSObject* stream = ::TransformStreamDefaultController::stream(self);
    JSObject* readable = TransformStream::readable(stream);
    double value;
    bool has_value;
    if (!JS::ReadableStreamGetDesiredSize(cx, readable, &has_value, &value)) {
      return false;
    }

    if (!has_value) {
      args.rval().setNull();
    } else {
      args.rval().set(JS_NumberValue(value));
    }

    return true;
  }

  const JSPropertySpec properties[] = {
    JS_PSG("desiredSize", desiredSize_get, 0),
  JS_PS_END};

  /**
   * https://streams.spec.whatwg.org/#ts-default-controller-enqueue
   */
  bool enqueue_js(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(0, "enqueue")

    // 1.  Perform TransformStreamDefaultControllerEnqueue([this], chunk).
    if (!Enqueue(cx, self, args.get(0))) {
      return false;
    }

    args.rval().setUndefined();
    return true;
  }

  /**
   * https://streams.spec.whatwg.org/#ts-default-controller-error
   */
  bool error_js(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(0, "error")

    // 1.  Perform TransformStreamDefaultControllerError(this, e).
    // (inlined)
    RootedObject stream(cx, ::TransformStreamDefaultController::stream(self));

    if (!TransformStream::Error(cx, stream, args.get(0))) {
      return false;
    }

    args.rval().setUndefined();
    return true;
  }

  /**
   * https://streams.spec.whatwg.org/#ts-default-controller-terminate
   */
  bool terminate_js(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(0, "terminate")

    // 1.  Perform TransformStreamDefaultControllerTerminate(this).
    if (!Terminate(cx, self)) {
      return false;
    }

    args.rval().setUndefined();
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("enqueue", enqueue_js, 1, 0),
    JS_FN("error", error_js, 1, 0),
    JS_FN("terminate", terminate_js, 0, 0),
  JS_FS_END};

  CLASS_BOILERPLATE_NO_CTOR(TransformStreamDefaultController)

  JSObject* create(JSContext* cx, HandleObject stream, TransformAlgorithm* transformAlgo,
                   FlushAlgorithm* flushAlgo)
  {
    RootedObject controller(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!controller) return nullptr;

    // 1.  Assert: stream [implements] `[TransformStream]`.
    MOZ_ASSERT(TransformStream::is_instance(stream));

    // 2.  Assert: stream.[controller] is undefined.
    MOZ_ASSERT(JS::GetReservedSlot(stream, TransformStream::Slots::Controller)
                   .isUndefined());

    // 3.  Set controller.[stream] to stream.
    JS::SetReservedSlot(controller, Slots::Stream, ObjectValue(*stream));

    // 4.  Set stream.[controller] to controller.
    JS::SetReservedSlot(stream, TransformStream::Slots::Controller,
                        ObjectValue(*controller));

    // 5.  Set controller.[transformAlgorithm] to transformAlgorithm.
    JS::SetReservedSlot(controller, Slots::TransformAlgorithm,
                        PrivateValue((void*)transformAlgo));

    // 6.  Set controller.[flushAlgorithm] to flushAlgorithm.
    JS::SetReservedSlot(controller, Slots::FlushAlgorithm, PrivateValue((void*)flushAlgo));

    return controller;
  }

  void set_transformer(JSObject* controller, Value transformer, JSObject* transformFunction,
                       JSObject* flushFunction)
  {
    JS::SetReservedSlot(controller, Slots::Transformer, transformer);
    JS::SetReservedSlot(controller, Slots::TransformInput,
                        ObjectOrNullValue(transformFunction));
    JS::SetReservedSlot(controller, Slots::FlushInput, ObjectOrNullValue(flushFunction));
  }

  /**
   * TransformStreamDefaultControllerEnqueue
   */
  bool Enqueue(JSContext* cx, HandleObject controller, HandleValue chunk) {
    MOZ_ASSERT(is_instance(controller));

    // 1.  Let stream be controller.[stream].
    RootedObject stream(cx, ::TransformStreamDefaultController::stream(controller));

    // 2.  Let readableController be stream.[readable].[controller].
    RootedObject readable(cx, TransformStream::readable(stream));
    RootedObject readableController(cx, JS::ReadableStreamGetController(cx, readable));
    MOZ_ASSERT(readableController);

    // 3.  If ! [ReadableStreamDefaultControllerCanCloseOrEnqueue](readableController) is false,
    // throw a `TypeError` exception.
    if (!JS::CheckReadableStreamControllerCanCloseOrEnqueue(cx, readableController, "enqueue")) {
      return false;
    }

    // 4.  Let enqueueResult be ReadableStreamDefaultControllerEnqueue(readableController, chunk).
    bool enqueueResult = JS::ReadableStreamEnqueue(cx, readable, chunk);

    // 5.  If enqueueResult is an abrupt completion,
    if (!enqueueResult) {
      // 5.1.  Perform
      // TransformStreamErrorWritableAndUnblockWrite(stream, enqueueResult.[Value]).
      RootedValue resultValue(cx);
      if (!JS_GetPendingException(cx, &resultValue)) {
        return false;
      }
      JS_ClearPendingException(cx);

      if (!TransformStream::ErrorWritableAndUnblockWrite(cx, stream, resultValue)) {
        return false;
      }

      //     2.  Throw stream.[readable].[storedError].
      RootedValue storedError(cx, JS::ReadableStreamGetStoredError(cx, readable));
      JS_SetPendingException(cx, storedError);
      return false;
    }

    // 6.  Let backpressure be ReadableStreamDefaultControllerHasBackpressure(readableController).
    // (Inlined)
    bool backpressure = !JS::ReadableStreamControllerShouldCallPull(cx, readableController);

    // 7.  If backpressure is not stream.[backpressure],
    if (backpressure != TransformStream::backpressure(stream)) {
      //     1.  Assert: backpressure is true.
      MOZ_ASSERT(backpressure);

      //     2.  Perform ! TransformStreamSetBackpressure(stream, true).
      TransformStream::SetBackpressure(cx, stream, true);
    }

    return true;
  }

  /**
   * TransformStreamDefaultControllerTerminate
   */
  bool Terminate(JSContext* cx, HandleObject controller) {
    MOZ_ASSERT(is_instance(controller));

    // 1.  Let stream be controller.[stream].
    RootedObject stream(cx, ::TransformStreamDefaultController::stream(controller));

    // 2.  Let readableController be stream.[readable].[controller].
    RootedObject readable(cx, TransformStream::readable(stream));
    RootedObject readableController(cx, JS::ReadableStreamGetController(cx, readable));
    MOZ_ASSERT(readableController);

    // 3.  Perform ! [ReadableStreamDefaultControllerClose](readableController).
    // Note: in https://github.com/whatwg/streams/pull/1029, the spec was changed to
    // make ReadableStreamDefaultControllerClose (and -Enqueue) return early if
    // ReadableStreamDefaultControllerCanCloseOrEnqueue is false. SpiderMonkey hasn't been
    // updated accordingly, so it'll throw an exception instead.
    // To avoid that, we do the check explicitly. While that also throws an exception, we can
    // just clear it and move on.
    // Note that this is future-proof: if SpiderMonkey is updated accordingly, it'll simply
    // stop throwing an exception, and the `else` branch will never be taken.
    if (JS::CheckReadableStreamControllerCanCloseOrEnqueue(cx, readableController, "close")) {
      if (!JS::ReadableStreamClose(cx, readable)) {
        return false;
      }
    } else {
      JS_ClearPendingException(cx);
    }

    // 4.  Let error be a `[TypeError](https://tc39.es/ecma262/#sec-native-error-types-used-in-this-standard-typeerror)` exception indicating that the stream has been terminated.
    // JSAPI doesn't allow us to create a proper error object with the right stack and all without
    // actually throwing it. So we do that and then immediately clear the pending exception.
    RootedValue error(cx);
    JS_ReportErrorLatin1(cx, "The TransformStream has been terminated");
    if (!JS_GetPendingException(cx, &error)) {
      return false;
    }
    JS_ClearPendingException(cx);

    // 5.  Perform ! [TransformStreamErrorWritableAndUnblockWrite](stream, error).
    return TransformStream::ErrorWritableAndUnblockWrite(cx, stream, error);
  }

  /**
   * Invoke the given callback in a way that treats it as a WebIDL callback returning `Promise<undefined>`, by first calling it and then running step 14 of
   * <invoke a callback function> and the conversion step from
   * https://webidl.spec.whatwg.org/#es-promise on the completion value.
   */
  JSObject* InvokePromiseReturningCallback(JSContext* cx, HandleValue receiver,
                                           HandleValue callback, JS::HandleValueArray args)
  {
    RootedValue rval(cx);
    if (!JS::Call(cx, receiver, callback, args, &rval)) {
      return PromiseRejectedWithPendingError(cx);
    }

    return JS::CallOriginalPromiseResolve(cx, rval);
  }

  /**
   * The TransformerAlgorithm to use for TransformStreams created using the JS constructor, with
   * or without a `transformer` passed in.
   *
   * Steps 2.* and 4 of SetUpTransformStreamDefaultControllerFromTransformer.
   */
  JSObject* transform_algorithm_transformer(JSContext* cx, HandleObject controller,
                                            HandleValue chunk)
  {
    MOZ_ASSERT(is_instance(controller));

    // Step 2.  Let transformAlgorithm be the following steps, taking a chunk argument:
    RootedValue transformFunction(cx);
    transformFunction = JS::GetReservedSlot(controller, Slots::TransformInput);
    if (!transformFunction.isObject()) {
      // 2.1.  Let result be TransformStreamDefaultControllerEnqueue(controller, chunk).
      if (!Enqueue(cx, controller, chunk)) {
        // 2.2.  If result is an abrupt completion, return a promise rejected with result.[Value].
        return PromiseRejectedWithPendingError(cx);
      }

      // 2.3.  Otherwise, return a promise resolved with undefined.
      return JS::CallOriginalPromiseResolve(cx, JS::UndefinedHandleValue);
    }

    // Step 4.  If transformerDict[transform] exists, set transformAlgorithm to an algorithm which
    // takes an argument chunk and returns the result of invoking transformerDict[transform] with
    // argument list « chunk, controller » and callback this value transformer.
    RootedValue transformer(cx, JS::GetReservedSlot(controller, Slots::Transformer));
    JS::RootedValueArray<2> newArgs(cx);
    newArgs[0].set(chunk);
    newArgs[1].setObject(*controller);
    return InvokePromiseReturningCallback(cx, transformer, transformFunction, newArgs);
  }

  /**
   * The FlushAlgorithm to use for TransformStreams created using the JS constructor, with
   * or without a `transformer` passed in.
   *
   * Steps 3 and 5 of SetUpTransformStreamDefaultControllerFromTransformer.
   */
  JSObject* flush_algorithm_transformer(JSContext* cx, HandleObject controller) {
    MOZ_ASSERT(is_instance(controller));

    // Step 3.  Let flushAlgorithm be an algorithm which returns a promise resolved with undefined.
    RootedValue flushFunction(cx, JS::GetReservedSlot(controller, Slots::FlushInput));
    if (!flushFunction.isObject()) {
      return JS::CallOriginalPromiseResolve(cx, JS::UndefinedHandleValue);
    }

    // Step 5.  If transformerDict[flush] exists, set flushAlgorithm to an algorithm which returns the result of invoking transformerDict[flush] with argument list « controller » and callback this value transformer.
    RootedValue transformer(cx, JS::GetReservedSlot(controller, Slots::Transformer));
    JS::RootedValueArray<1> newArgs(cx);
    newArgs[0].setObject(*controller);
    return InvokePromiseReturningCallback(cx, transformer, flushFunction, newArgs);
  }

  /**
   * SetUpTransformStreamDefaultController
   * https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller
   */
  JSObject* SetUp(JSContext* cx, HandleObject stream,
                  TransformAlgorithm* transformAlgo, FlushAlgorithm* flushAlgo)
  {
    MOZ_ASSERT(TransformStream::is_instance(stream));

    // Step 1 of SetUpTransformStreamDefaultControllerFromTransformer and step 1-6 of this
    // algorithm.
    RootedObject controller(cx);
    controller = TransformStreamDefaultController::create(cx, stream, transformAlgo, flushAlgo);
    return controller;
  }

  /**
   * SetUpTransformStreamDefaultControllerFromTransformer
   * https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller-from-transformer
   */
  JSObject* SetUpFromTransformer(JSContext* cx, HandleObject stream, HandleValue transformer,
                                 HandleObject transformFunction, HandleObject flushFunction)
  {
    MOZ_ASSERT(TransformStream::is_instance(stream));

    // Step 1, moved into SetUpTransformStreamDefaultController.
    // Step 6.  Perform ! [SetUpTransformStreamDefaultController](stream, controller, transformAlgorithm, flushAlgorithm).
    RootedObject controller(cx);
    controller = SetUp(cx, stream, transform_algorithm_transformer, flush_algorithm_transformer);
    if (!controller) return nullptr;

    // Set the additional bits required to execute the transformer-based transform and flush
    // algorithms.
    set_transformer(controller, transformer, transformFunction, flushFunction);

    // Steps 2-5 implemented in dedicated functions above.
    return controller;
  }

  /**
   * Steps 2.* of TransformStreamDefaultControllerPerformTransform.
   */
  bool transformPromise_catch_handler(JSContext* cx, HandleObject controller, CallArgs args) {
    RootedValue r(cx, args.get(0));
    //     1.  Perform ! [TransformStreamError](controller.[stream], r).
    RootedObject streamObj(cx, stream(controller));
    if (!TransformStream::Error(cx, streamObj, r)) {
      return false;
    }

    //     2.  Throw r.
    JS_SetPendingException(cx, r);
    return false;
  }

  /**
   * TransformStreamDefaultControllerPerformTransform
   */
  JSObject* PerformTransform(JSContext* cx, HandleObject controller, HandleValue chunk) {
    MOZ_ASSERT(is_instance(controller));

    // 1.  Let transformPromise be the result of performing controller.[transformAlgorithm], passing chunk.
    TransformAlgorithm* transformAlgo = transformAlgorithm(controller);
    RootedObject transformPromise(cx, transformAlgo(cx, controller, chunk));
    if (!transformPromise) {
      return nullptr;
    }

    // 2.  Return the result of reacting to transformPromise with the following rejection
    // steps given the argument r:
    RootedObject catch_handler(cx);
    catch_handler = create_internal_method<transformPromise_catch_handler>(cx, controller);
    if (!catch_handler) {
      return nullptr;
    }

    return JS::CallOriginalPromiseThen(cx, transformPromise, nullptr, catch_handler);
  }

  /**
   * TransformStreamDefaultControllerClearAlgorithms
   */
  void ClearAlgorithms(JSObject* controller) {
    MOZ_ASSERT(is_instance(controller));

    // 1.  Set controller.[transformAlgorithm] to undefined.
    JS::SetReservedSlot(controller, Slots::TransformAlgorithm, PrivateValue(nullptr));
    JS::SetReservedSlot(controller, Slots::TransformInput, JS::UndefinedValue());

    // 2.  Set controller.[flushAlgorithm] to undefined.
    JS::SetReservedSlot(controller, Slots::FlushAlgorithm, PrivateValue(nullptr));
    JS::SetReservedSlot(controller, Slots::FlushInput, JS::UndefinedValue());
  }
}

bool ExtractFunction(JSContext* cx, HandleObject obj, const char* name,
                     JS::MutableHandleObject func)
{
  RootedValue val(cx);
  if (!JS_GetProperty(cx, obj, name, &val)) {
    return false;
  }

  if (val.isUndefined()) {
    return true;
  }

  if (!val.isObject() || !JS::IsCallable(&val.toObject())) {
    JS_ReportErrorLatin1(cx, "%s should be a function", name);
    return false;
  }

  func.set(&val.toObject());
  return true;
}

bool ExtractStrategy(JSContext* cx, HandleValue strategy, double default_hwm,
                     double* hwm, JS::MutableHandleFunction size)
{
  if (strategy.isUndefined()) {
    *hwm = default_hwm;
    return true;
  }

  if (!strategy.isObject()) {
    JS_ReportErrorLatin1(cx,
                         "Strategy passed to TransformStream constructor must be an object");
    return false;
  }

  RootedObject strategy_obj(cx, &strategy.toObject());

  RootedValue val(cx);
  if (!JS_GetProperty(cx, strategy_obj, "highWaterMark", &val)) {
    return false;
  }

  if (val.isUndefined()) {
    *hwm = default_hwm;
  } else {
    if (!JS::ToNumber(cx, val, hwm)) {
      return false;
    }
    if (mozilla::IsNaN(*hwm) || *hwm < 0) {
      JS_ReportErrorLatin1(cx, "Invalid value for highWaterMark: %f", *hwm);
      return false;
    }
  }

  RootedObject size_obj(cx);
  if (!ExtractFunction(cx, strategy_obj, "size", &size_obj)) {
    return false;
  }

  // JSAPI wants JSHandleFunction instances for the size algorithm, so that's what it'll get.
  if (size_obj) {
    val.setObjectOrNull(size_obj);
    size.set(JS_ValueToFunction(cx, val));
  }

  return true;
}

/**
 * Implementation of the WHATWG TransformStream builtin.
 *
 * All algorithm names and steps refer to spec algorithms defined at
 * https://streams.spec.whatwg.org/#ts-class
 */
namespace TransformStream {
  JSObject* create(JSContext* cx, double writableHighWaterMark,
                   JS::HandleFunction writableSizeAlgorithm,
                   double readableHighWaterMark,
                   JS::HandleFunction readableSizeAlgorithm,
                   HandleValue transformer, HandleObject startFunction,
                   HandleObject transformFunction, HandleObject flushFunction);

  /**
   * https://streams.spec.whatwg.org/#ts-constructor
   */
  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject startFunction(cx);
    RootedObject transformFunction(cx);
    RootedObject flushFunction(cx);

    // 1.  If transformer is missing, set it to null.
    RootedValue transformer(cx, args.get(0));
    if (transformer.isUndefined()) {
      transformer.setNull();
    }

    if (transformer.isObject()) {
      RootedObject transformerDict(cx, &transformer.toObject());

      // 2.  Let transformerDict be transformer, [converted to an IDL value] of type `
      //     [Transformer]`.
      // Note: we do the extraction of dict entries manually, because no WebIDL codegen.
      if (!ExtractFunction(cx, transformerDict, "start", &startFunction)) {
        return false;
      }

      if (!ExtractFunction(cx, transformerDict, "transform", &transformFunction)) {
        return false;
      }

      if (!ExtractFunction(cx, transformerDict, "flush", &flushFunction)) {
        return false;
      }

      // 3.  If transformerDict["readableType"] [exists], throw a `[RangeError]` exception.
      bool found;
      if (!JS_HasProperty(cx, transformerDict, "readableType", &found)) {
        return false;
      }
      if (found) {
        JS_ReportErrorLatin1(cx, "transformer.readableType is reserved for future use");
        return false;
      }

      // 4.  If transformerDict["writableType"] [exists], throw a `[RangeError]` exception.
      if (!JS_HasProperty(cx, transformerDict, "writableType", &found)) {
        return false;
      }
      if (found) {
        JS_ReportErrorLatin1(cx, "transformer.writableType is reserved for future use");
        return false;
      }
  }

    // 5.  Let readableHighWaterMark be ? [ExtractHighWaterMark](readableStrategy, 0).
    // 6.  Let readableSizeAlgorithm be ! [ExtractSizeAlgorithm](readableStrategy).
    double readableHighWaterMark;
    JS::RootedFunction readableSizeAlgorithm(cx);
    if (!ExtractStrategy(cx, args.get(2), 0, &readableHighWaterMark,
                         &readableSizeAlgorithm))
    {
      return false;
    }

    // 7.  Let writableHighWaterMark be ? [ExtractHighWaterMark](writableStrategy, 1).
    // 8.  Let writableSizeAlgorithm be ! [ExtractSizeAlgorithm](writableStrategy).
    double writableHighWaterMark;
    JS::RootedFunction writableSizeAlgorithm(cx);
    if (!ExtractStrategy(cx, args.get(1), 1, &writableHighWaterMark,
                         &writableSizeAlgorithm))
    {
      return false;
    }

    // Steps 9-13.
    RootedObject self(cx, create(cx, writableHighWaterMark, writableSizeAlgorithm,
                                 readableHighWaterMark, readableSizeAlgorithm,
                                 transformer, startFunction, transformFunction,
                                 flushFunction));
    if (!self) return false;

    args.rval().setObject(*self);
    return true;
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  /**
   * The native object owning the sink underlying the TransformStream's readable end.
   *
   * This can e.g. be a RequestOrResponse if the readable is used as a body.
   */
  JSObject* owner(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return &JS::GetReservedSlot(self, Slots::Owner).toObject();
  }

  void set_owner(JSObject* self, JSObject* owner) {
    MOZ_ASSERT(is_instance(self));
    MOZ_ASSERT(RequestOrResponse::is_instance(owner));
    MOZ_ASSERT(JS::GetReservedSlot(self, Slots::Owner).isUndefined());
    JS::SetReservedSlot(self, Slots::Owner, ObjectValue(*owner));
  }

  JSObject* readable(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return &JS::GetReservedSlot(self, Slots::Readable).toObject();
  }

  bool is_ts_readable(JSContext* cx, HandleObject readable) {
    JSObject* source = NativeStreamSource::get_stream_source(cx, readable);
    if (!source || !NativeStreamSource::is_instance(source)) {
      return false;
    }
    JSObject* stream_owner = NativeStreamSource::owner(source);
    return stream_owner ? TransformStream::is_instance(stream_owner) : false;
  }

  bool readable_used_as_body(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    // For now the owner can only be a RequestOrResponse, so no further checks are needed.
    return JS::GetReservedSlot(self, Slots::Owner).isObject();
  }

  /**
   * Sets the |target| RequestOrResponse object as the owner of the TransformStream
   * |readable| is the readable end of.
   *
   * This allows us to later on short-cut piping from native body to native body.
   *
   * Asserts that |readable| is the readable end of a TransformStream.
   */
  void set_readable_used_as_body(JSContext* cx, HandleObject readable, HandleObject target) {
    MOZ_ASSERT(is_ts_readable(cx, readable));
    JSObject* source = NativeStreamSource::get_stream_source(cx, readable);
    JSObject* stream_owner = NativeStreamSource::owner(source);
    set_owner(stream_owner, target);
  }

  JSObject* writable(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return &JS::GetReservedSlot(self, Slots::Writable).toObject();
  }

  bool is_ts_writable(JSContext* cx, HandleObject writable) {
    JSObject* sink = NativeStreamSink::get_stream_sink(cx, writable);
    if (!sink || !NativeStreamSink::is_instance(sink)) {
      return false;
    }
    JSObject* stream_owner = NativeStreamSink::owner(sink);
    return stream_owner ? is_instance(stream_owner) : false;
  }

  JSObject* controller(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return &JS::GetReservedSlot(self, Slots::Controller).toObject();
  }

  bool backpressure(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return JS::GetReservedSlot(self, Slots::Backpressure).toBoolean();
  }

  JSObject* backpressureChangePromise(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return JS::GetReservedSlot(self, Slots::BackpressureChangePromise).toObjectOrNull();
  }

  bool readable_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(0, "get readable")
    args.rval().setObject(*readable(self));
    return true;
  }

  bool writable_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(0, "get writable")
    args.rval().setObject(*writable(self));
    return true;
  }

  const JSFunctionSpec methods[] = {JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("readable", readable_get, 0),
    JS_PSG("writable", writable_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE_CUSTOM_INIT(TransformStream)

  bool init_class(JSContext* cx, HandleObject global) {
    bool ok = init_class_impl(cx, global);
    if (!ok) return false;

    return ReadableStream_additions::initialize_additions(cx, global);
  }

  /**
   * TransformStreamError
   */
  bool Error(JSContext* cx, HandleObject stream, HandleValue error) {
    MOZ_ASSERT(is_instance(stream));

    // 1.  Perform ReadableStreamDefaultControllerError(stream.[readable].[controller], e).
    RootedObject readable(cx, ::TransformStream::readable(stream));
    if (!JS::ReadableStreamError(cx, readable, error)) {
      return false;
    }

    // 2.  Perform ! [TransformStreamErrorWritableAndUnblockWrite](stream, e).
    return ErrorWritableAndUnblockWrite(cx, stream, error);
  }

  /**
   * TransformStreamDefaultSourcePullAlgorithm
   */
  bool DefaultSourcePullAlgorithm(JSContext* cx, CallArgs args, HandleObject readable,
                                  HandleObject stream, HandleObject controller)
  {
    // 1.  Assert: stream.[backpressure] is true.
    MOZ_ASSERT(backpressure(stream));

    // 2.  Assert: stream.[backpressureChangePromise] is not undefined.
    MOZ_ASSERT(backpressureChangePromise(stream));

    // 3.  Perform ! [TransformStreamSetBackpressure](stream, false).
    if (!SetBackpressure(cx, stream, false)) {
      return false;
    }

    // 4.  Return stream.[backpressureChangePromise].
    args.rval().setObject(*backpressureChangePromise(stream));
    return true;
  }

  /**
   * Steps 7.* of InitializeTransformStream
   */
  bool DefaultSourceCancelAlgorithm(JSContext* cx, CallArgs args, HandleObject readable,
                                    HandleObject stream, HandleValue reason)
  {
    MOZ_ASSERT(is_instance(stream));

    // 1.  Perform ! [TransformStreamErrorWritableAndUnblockWrite](stream, reason).
    if (!ErrorWritableAndUnblockWrite(cx, stream, reason)) {
      return false;
    }

    // 2.  Return [a promise resolved with] undefined.
    args.rval().setUndefined();
    return true;
  }

  /**
   * Steps 2 and 3.* of DefaultSinkWriteAlgorithm.
   */
  bool default_sink_write_algo_then_handler(JSContext* cx, HandleObject stream,
                                            CallArgs args)
  {
    RootedObject then_handler(cx, &args.callee());
    RootedValue chunk(cx, js::GetFunctionNativeReserved(then_handler, 1));

    // 3.1.  Let writable be stream.[writable].
    RootedObject writable(cx, ::TransformStream::writable(stream));

    // 3.2.  Let state be writable.[state].
    auto state = JS::WritableStreamGetState(cx, writable);

    // 3.3.  If state is "`erroring`", throw writable.[storedError].
    if (state == JS::WritableStreamState::Erroring) {
      RootedValue storedError(cx, JS::WritableStreamGetStoredError(cx, writable));
      JS_SetPendingException(cx, storedError);
      return false;
    }

    // 3.4.  Assert: state is "`writable`".
    MOZ_ASSERT(state == JS::WritableStreamState::Writable);

    // 2.  Let controller be stream.[controller].
    RootedObject controller(cx, TransformStream::controller(stream));

    // 3.5.  Return TransformStreamDefaultControllerPerformTransform(controller, chunk).
    RootedObject transformPromise(cx);
    transformPromise = TransformStreamDefaultController::PerformTransform(cx, controller, chunk);
    if (!transformPromise) {
      return false;
    }

    args.rval().setObject(*transformPromise);
    return true;
  }

  bool DefaultSinkWriteAlgorithm(JSContext* cx, CallArgs args, HandleObject writableController,
                                 HandleObject stream, HandleValue chunk)
  {
    RootedObject writable(cx, ::TransformStream::writable(stream));

    // 1.  Assert: stream.[writable].[state] is "`writable`".
    MOZ_ASSERT(JS::WritableStreamGetState(cx, writable) ==
               JS::WritableStreamState::Writable);

    // 2. (reordered below)

    // 3.  If stream.[backpressure] is true,
    if (TransformStream::backpressure(stream)) {
      //     1.  Let backpressureChangePromise be stream.[backpressureChangePromise].
      RootedObject changePromise(cx, TransformStream::backpressureChangePromise(stream));

      //     2.  Assert: backpressureChangePromise is not undefined.
      MOZ_ASSERT(changePromise);

      //     3.  Return the result of [reacting] to backpressureChangePromise with the following
      //         fulfillment steps:
      RootedObject then_handler(cx);
      then_handler = create_internal_method<default_sink_write_algo_then_handler>(cx, stream);
      if (!then_handler) return false;

      // Store the chunk value on the then_handler function for later retrieval.
      js::SetFunctionNativeReserved(then_handler, 1, chunk);

      RootedObject result(cx);
      result = JS::CallOriginalPromiseThen(cx, changePromise, then_handler, nullptr);
      if (!result) {
        return false;
      }

      args.rval().setObject(*result);
      return true;
    }

    // 2.  Let controller be stream.[controller].
    RootedObject controller(cx, TransformStream::controller(stream));

    // 4.  Return ! [TransformStreamDefaultControllerPerformTransform](controller, chunk).
    RootedObject transformPromise(cx);
    transformPromise = TransformStreamDefaultController::PerformTransform(cx, controller, chunk);
    if (!transformPromise) {
      return ReturnPromiseRejectedWithPendingError(cx, args);
    }

    args.rval().setObject(*transformPromise);
    return true;
  }

  bool DefaultSinkAbortAlgorithm(JSContext* cx, CallArgs args,
                                 HandleObject writableController, HandleObject stream,
                                 HandleValue reason)
  {
    MOZ_ASSERT(is_instance(stream));

    // 1.  Perform ! [TransformStreamError](stream, reason).
    if (!Error(cx, stream, reason)) {
      return false;
    }

    // 2.  Return [a promise resolved with] undefined.
    RootedObject promise(cx, JS::CallOriginalPromiseResolve(cx, JS::UndefinedHandleValue));
    if (!promise) {
      return false;
    }

    args.rval().setObject(*promise);
    return true;
  }

  /**
   * Steps 5.1.1-2 of DefaultSinkCloseAlgorithm.
   */
  bool default_sink_close_algo_then_handler(JSContext* cx, HandleObject stream, CallArgs args) {
    // 5.1.1.  If readable.[state] is "`errored`", throw readable.[storedError].
    RootedObject then_handler(cx, &args.callee());
    RootedObject readable(cx, &js::GetFunctionNativeReserved(then_handler, 1).toObject());
    bool is_errored;
    if (!JS::ReadableStreamIsErrored(cx, readable, &is_errored)) {
      return false;
    }

    if (is_errored) {
      RootedValue storedError(cx, JS::ReadableStreamGetStoredError(cx, readable));
      JS_SetPendingException(cx, storedError);
      return false;
    }

    // 5.1.2.  Perform ! [ReadableStreamDefaultControllerClose](readable.[controller]).
    RootedObject readableController(cx, JS::ReadableStreamGetController(cx, readable));
    // Note: in https://github.com/whatwg/streams/pull/1029, the spec was changed to
    // make ReadableStreamDefaultControllerClose (and -Enqueue) return early if
    // ReadableStreamDefaultControllerCanCloseOrEnqueue is false. SpiderMonkey hasn't been
    // updated accordingly, so it'll throw an exception instead.
    // To avoid that, we do the check explicitly. While that also throws an exception, we can
    // just clear it and move on.
    // Note that this is future-proof: if SpiderMonkey is updated accordingly, it'll simply
    // stop throwing an exception, and the `else` branch will never be taken.
    if (JS::CheckReadableStreamControllerCanCloseOrEnqueue(cx, readableController, "close")) {
      return JS::ReadableStreamClose(cx, readable);
    } else {
      JS_ClearPendingException(cx);
    }
    return true;
  }

  /**
   * Steps 5.2.1-2 of DefaultSinkCloseAlgorithm.
   */
  bool default_sink_close_algo_catch_handler(JSContext* cx, HandleObject stream, CallArgs args) {
    // 5.2.1.  Perform ! [TransformStreamError](stream, r).
    HandleValue r = args.get(0);
    if (!Error(cx, stream, r)) {
      return false;
    }

    // 5.2.2.  Throw readable.[storedError].
    RootedObject then_handler(cx, &args.callee());
    RootedObject readable(cx, &js::GetFunctionNativeReserved(then_handler, 1).toObject());
    RootedValue storedError(cx, JS::ReadableStreamGetStoredError(cx, readable));
    JS_SetPendingException(cx, storedError);
    return false;
  }

  bool DefaultSinkCloseAlgorithm(JSContext* cx, CallArgs args,
                                 HandleObject writableController, HandleObject stream)
  {
    MOZ_ASSERT(is_instance(stream));

    // 1.  Let readable be stream.[readable].
    RootedObject readable(cx, ::TransformStream::readable(stream));

    // 2.  Let controller be stream.[controller].
    RootedObject controller(cx, ::TransformStream::controller(stream));

    // 3.  Let flushPromise be the result of performing controller.[flushAlgorithm].
    auto flushAlgorithm = TransformStreamDefaultController::flushAlgorithm(controller);
    RootedObject flushPromise(cx, flushAlgorithm(cx, controller));
    if (!flushPromise) {
      return false;
    }

    // 4.  Perform ! [TransformStreamDefaultControllerClearAlgorithms](controller).
    TransformStreamDefaultController::ClearAlgorithms(controller);

    // 5.  Return the result of [reacting] to flushPromise:
    // 5.1.  If flushPromise was fulfilled, then:
    // Sub-steps in handler above.
    RootedObject then_handler(cx);
    then_handler = create_internal_method<default_sink_close_algo_then_handler>(cx, stream);
    if (!then_handler) return false;

    // Store the readable on the then_handler function for later retrieval.
    js::SetFunctionNativeReserved(then_handler, 1, ObjectValue(*readable));

    // 5.2.  If flushPromise was rejected with reason r, then:
    // Sub-steps in handler above.
    RootedObject catch_handler(cx);
    catch_handler = create_internal_method<default_sink_close_algo_catch_handler>(cx, stream);
    if (!catch_handler) return false;

    // Store the readable on the catch_handler function for later retrieval.
    js::SetFunctionNativeReserved(catch_handler, 1, ObjectValue(*readable));

    RootedObject result(cx);
    result = JS::CallOriginalPromiseThen(cx, flushPromise, then_handler, catch_handler);
    if (!result) {
      return false;
    }

    args.rval().setObject(*result);
    return true;
  }

  /**
   * TransformStreamSetBackpressure
   */
  bool SetBackpressure(JSContext* cx, HandleObject stream, bool backpressure) {
    // 1.  Assert: stream.[backpressure] is not backpressure.
    MOZ_ASSERT(::TransformStream::backpressure(stream) != backpressure);

    // 2.  If stream.[backpressureChangePromise] is not undefined, resolve stream.[backpressureChangePromise] with undefined.
    RootedObject changePromise(cx, backpressureChangePromise(stream));
    if (changePromise) {
      if (!JS::ResolvePromise(cx, changePromise, JS::UndefinedHandleValue)) {
        return false;
      }
    }

    // 3.  Set stream.[backpressureChangePromise] to a new promise.
    changePromise = JS::NewPromiseObject(cx, nullptr);
    if (!changePromise) {
      return false;
    }
    JS::SetReservedSlot(stream, Slots::BackpressureChangePromise, ObjectValue(*changePromise));

    // 4.  Set stream.[backpressure] to backpressure.
    JS::SetReservedSlot(stream, Slots::Backpressure, JS::BooleanValue(backpressure));

    return true;
  }

  /**
   * https://streams.spec.whatwg.org/#initialize-transform-stream
   * Steps 9-13.
   */
  bool Initialize(JSContext* cx, HandleObject stream, HandleObject startPromise,
                  double writableHighWaterMark, JS::HandleFunction writableSizeAlgorithm,
                  double readableHighWaterMark, JS::HandleFunction readableSizeAlgorithm)
  {
    // Step 1.  Let startAlgorithm be an algorithm that returns startPromise.
    // (Inlined)

    // Steps 2-4 implemented as DefaultSink*Algorithm functions above.

    // Step 5.  Set stream.[writable] to ! [CreateWritableStream](startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, writableHighWaterMark, writableSizeAlgorithm).
    RootedValue startPromiseVal(cx, ObjectValue(*startPromise));
    RootedObject sink(cx, NativeStreamSink::create(cx, stream, startPromiseVal,
                                                   DefaultSinkWriteAlgorithm,
                                                   DefaultSinkCloseAlgorithm,
                                                   DefaultSinkAbortAlgorithm));
    if (!sink) return false;

    RootedObject writable(cx);
    writable = JS::NewWritableDefaultStreamObject(cx, sink, writableSizeAlgorithm,
                                                  writableHighWaterMark);
    if (!writable) return false;

    JS::SetReservedSlot(stream, Slots::Writable, ObjectValue(*writable));

    // Step 6.  Let pullAlgorithm be the following steps:
    auto pullAlgorithm = DefaultSourcePullAlgorithm;

    // Step 7.  Let cancelAlgorithm be the following steps, taking a reason argument:
    // (Sub-steps moved into DefaultSourceCancelAlgorithm)
    auto cancelAlgorithm = DefaultSourceCancelAlgorithm;

    // Step 8.  Set stream.[readable] to ! [CreateReadableStream](startAlgorithm, pullAlgorithm, cancelAlgorithm, readableHighWaterMark, readableSizeAlgorithm).
    RootedObject source(cx, NativeStreamSource::create(cx, stream, startPromiseVal,
                                                       pullAlgorithm, cancelAlgorithm));
    if (!source) return false;

    RootedObject readable(cx, JS::NewReadableDefaultStreamObject(cx, source,
                                                                 readableSizeAlgorithm,
                                                                 readableHighWaterMark));
    if (!readable) return false;

    JS::SetReservedSlot(stream, Slots::Readable, ObjectValue(*readable));

    // Step 9.  Set stream.[backpressure] and stream.[backpressureChangePromise] to undefined.
    // As the note in the spec says, it's valid to instead set `backpressure` to a
    // boolean value early, which makes implementing SetBackpressure easier.
    JS::SetReservedSlot(stream, Slots::Backpressure, JS::FalseValue());

    // For similar reasons, ensure that the backpressureChangePromise slot is null.
    JS::SetReservedSlot(stream, Slots::BackpressureChangePromise, JS::NullValue());

    // Step 10. Perform ! [TransformStreamSetBackpressure](stream, true).
    if (!SetBackpressure(cx, stream, true)) {
      return false;
    }

    // Step 11. Set stream.[controller] to undefined.
    JS::SetReservedSlot(stream, Slots::Controller, JS::UndefinedValue());

    return true;
  }

  bool ErrorWritableAndUnblockWrite(JSContext* cx, HandleObject stream, HandleValue error) {
    MOZ_ASSERT(is_instance(stream));

    // 1.  Perform TransformStreamDefaultControllerClearAlgorithms(stream.[controller]).
    TransformStreamDefaultController::ClearAlgorithms(controller(stream));

    // 2.  Perform WritableStreamDefaultControllerErrorIfNeeded(stream.[writable].[controller], e).
    // (inlined)
    RootedObject writable(cx, ::TransformStream::writable(stream));
    if (JS::WritableStreamGetState(cx, writable) == JS::WritableStreamState::Writable) {
      if (!JS::WritableStreamError(cx, writable, error)) {
        return false;
      }
    }

    // 3.  If stream.[backpressure] is true, perform TransformStreamSetBackpressure(stream, false).
    if (backpressure(stream)) {
      if (!SetBackpressure(cx, stream, false)) {
        return false;
      }
    }

    return true;
  }

  /**
   * https://streams.spec.whatwg.org/#ts-constructor
   * Steps 9-13.
   */
  JSObject* create(JSContext* cx, double writableHighWaterMark,
                   JS::HandleFunction writableSizeAlgorithm,
                   double readableHighWaterMark,
                   JS::HandleFunction readableSizeAlgorithm,
                   HandleValue transformer, HandleObject startFunction,
                   HandleObject transformFunction, HandleObject flushFunction)
  {
    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    // Step 9.
    RootedObject startPromise(cx, JS::NewPromiseObject(cx, nullptr));
    if (!startPromise) return nullptr;

    // Step 10.
    if (!Initialize(cx, self, startPromise, writableHighWaterMark, writableSizeAlgorithm,
                    readableHighWaterMark, readableSizeAlgorithm))
    {
      return nullptr;
    }

    // Step 11.
    RootedObject controller(cx);
    controller = TransformStreamDefaultController::SetUpFromTransformer(cx, self,
                                                                        transformer,
                                                                        transformFunction,
                                                                        flushFunction);
    if (!controller) {
      return nullptr;
    }

    RootedValue rval(cx);

    // Step 12.
    // If transformerDict["start"], then resolve startPromise with the result of invoking
    // transformerDict["start"] with argument list « this.[[[controller]]] » and callback
    // this value transformer.
    if (startFunction) {
      JS::RootedValueArray<1> newArgs(cx);
      newArgs[0].setObject(*controller);
      if (!JS::Call(cx, transformer, startFunction, newArgs, &rval)) {
        return nullptr;
      }
    }

    // Step 13.
    // Otherwise, resolve startPromise with undefined.
    if (!JS::ResolvePromise(cx, startPromise, rval)) {
      return nullptr;
    }

    return self;
  }
}

namespace CacheOverride {

  // The values stored in these slots are ultimately passed to the host
  // via the xqd_req_cache_override_v2_set hostcall.
  //
  // If `Mode` is not `Override`, all other values are ignored.
  //
  // If `Mode` is `Override`, the values are interpreted in the following way:
  //
  // If `TTL`, `SWR`, or `SurrogateKey` are `undefined`, they're ignored.
  // For each of them, if the value isn't `undefined`, a flag gets set in the
  // hostcall's `tag` parameter, and the value itself is encoded as a uint32
  // parameter.
  //
  // `PCI` is interpreted as a boolean, and a flag gets set in the hostcall's
  // `tag` parameter if `PCI` is true.
  namespace Slots { enum {
    Mode,
    TTL,
    SWR,
    SurrogateKey,
    PCI,
    Count
  };};

  enum class Mode {
    None,
    Pass,
    Override
  };

  // These values are defined by the Fastly ABI:
  // https://docs.rs/fastly-shared/0.6.1/src/fastly_shared/lib.rs.html#407-412
  enum class CacheOverrideTag {
    None = 0,
    Pass = 1 << 0,
    TTL = 1 << 1,
    SWR = 1 << 2,
    PCI = 1 << 3,
  };

  Mode mode(JSObject* self) {
    return (Mode)JS::GetReservedSlot(self, Slots::Mode).toInt32();
  }

  void set_mode(JSObject* self, Mode mode) {
    JS::SetReservedSlot(self, Slots::Mode, JS::Int32Value((int32_t)mode));
  }

  JS::Value ttl(JSObject* self) {
    if (mode(self) != Mode::Override)
      return JS::UndefinedValue();
    return JS::GetReservedSlot(self, Slots::TTL);
  }

  void set_ttl(JSObject* self, uint32_t ttl) {
    MOZ_RELEASE_ASSERT(mode(self) == Mode::Override);
    JS::SetReservedSlot(self, Slots::TTL, JS::Int32Value((int32_t)ttl));
  }

  JS::Value swr(JSObject* self) {
    if (mode(self) != Mode::Override)
      return JS::UndefinedValue();
    return JS::GetReservedSlot(self, Slots::SWR);
  }

  void set_swr(JSObject* self, uint32_t swr) {
    MOZ_RELEASE_ASSERT(mode(self) == Mode::Override);
    JS::SetReservedSlot(self, Slots::SWR, JS::Int32Value((int32_t)swr));
  }

  JS::Value surrogate_key(JSObject* self) {
    if (mode(self) != Mode::Override)
      return JS::UndefinedValue();
    return JS::GetReservedSlot(self, Slots::SurrogateKey);
  }

  void set_surrogate_key(JSObject* self, JSString* key) {
    MOZ_RELEASE_ASSERT(mode(self) == Mode::Override);
    JS::SetReservedSlot(self, Slots::SurrogateKey, JS::StringValue(key));
  }

  JS::Value pci(JSObject* self) {
    if (mode(self) != Mode::Override)
      return JS::UndefinedValue();
    return JS::GetReservedSlot(self, Slots::PCI);
  }

  void set_pci(JSObject* self, bool pci) {
    MOZ_RELEASE_ASSERT(mode(self) == Mode::Override);
    JS::SetReservedSlot(self, Slots::PCI, JS::BooleanValue(pci));
  }

  uint32_t abi_tag(JSObject* self) {
    switch (mode(self)) {
      case Mode::None: return (uint32_t)CacheOverrideTag::None;
      case Mode::Pass: return (uint32_t)CacheOverrideTag::Pass;
      default:;
    }

    uint32_t tag = 0;
    if (!ttl(self).isUndefined())
      tag |= (uint32_t)CacheOverrideTag::TTL;
    if (!swr(self).isUndefined())
      tag |= (uint32_t)CacheOverrideTag::SWR;
    if (!pci(self).isUndefined())
      tag |= (uint32_t)CacheOverrideTag::PCI;

    return tag;
  }

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool mode_get(JSContext* cx, HandleObject self, MutableHandleValue rval) {
    const char* mode_chars;
    switch (mode(self)) {
      case Mode::None:
        mode_chars = "none"; break;
      case Mode::Pass:
        mode_chars = "pass"; break;
      case Mode::Override:
        mode_chars = "override"; break;
    }

    RootedString mode_str(cx, JS_NewStringCopyZ(cx, mode_chars));
    if (!mode_str) return false;

    rval.setString(mode_str);
    return true;
  }

  bool ensure_override(JSContext* cx, HandleObject self, const char* field) {
    if (mode(self) == Mode::Override)
      return true;

    JS_ReportErrorUTF8(cx, "Can't set %s on CacheOverride object whose mode "
                           "isn't \"override\"", field);
    return false;
  }

  bool mode_set(JSContext* cx, HandleObject self, HandleValue val, MutableHandleValue rval) {
    size_t mode_len;
    UniqueChars mode_chars = encode(cx, val, &mode_len);
    if (!mode_chars) return false;

    Mode mode;
    if (!strcmp(mode_chars.get(), "none")) {
      mode = Mode::None;
    } else if (!strcmp(mode_chars.get(), "pass")) {
      mode = Mode::Pass;
    } else if (!strcmp(mode_chars.get(), "override")) {
      mode = Mode::Override;
    } else {
      JS_ReportErrorUTF8(cx, "'mode' has to be \"none\", \"pass\", or \"override\", "
                             "but got %s", mode_chars.get());
      return false;
    }

    set_mode(self, mode);
    return true;
  }

  bool ttl_get(JSContext* cx, HandleObject self, MutableHandleValue rval) {
    rval.set(ttl(self));
    return true;
  }

  bool ttl_set(JSContext* cx, HandleObject self, HandleValue val, MutableHandleValue rval) {
    if (!ensure_override(cx, self, "a TTL"))
      return false;

    if (val.isUndefined()) {
      JS::SetReservedSlot(self, Slots::TTL, val);
    } else {
      int32_t ttl;
      if (!JS::ToInt32(cx, val, &ttl))
        return false;

      set_ttl(self, ttl);
    }
    rval.set(CacheOverride::ttl(self));
    return true;
  }

  bool swr_get(JSContext* cx, HandleObject self, MutableHandleValue rval) {
    rval.set(swr(self));
    return true;
  }

  bool swr_set(JSContext* cx, HandleObject self, HandleValue val, MutableHandleValue rval) {
    if (!ensure_override(cx, self, "SWR"))
      return false;

    if (val.isUndefined()) {
      JS::SetReservedSlot(self, Slots::SWR, val);
    } else {
      int32_t swr;
      if (!JS::ToInt32(cx, val, &swr))
        return false;

      set_swr(self, swr);
    }
    rval.set(CacheOverride::swr(self));
    return true;
  }

  bool surrogate_key_get(JSContext* cx, HandleObject self, MutableHandleValue rval) {
    rval.set(surrogate_key(self));
    return true;
  }

  bool surrogate_key_set(JSContext* cx, HandleObject self, HandleValue val, MutableHandleValue rval) {
    if (!ensure_override(cx, self, "a surrogate key"))
      return false;

    if (val.isUndefined()) {
      JS::SetReservedSlot(self, Slots::SurrogateKey, val);
    } else {
      RootedString surrogate_key(cx, JS::ToString(cx, val));
      if (!surrogate_key) return false;

      set_surrogate_key(self, surrogate_key);
    }
    rval.set(CacheOverride::surrogate_key(self));
    return true;
  }

  bool pci_get(JSContext* cx, HandleObject self, MutableHandleValue rval) {
    rval.set(pci(self));
    return true;
  }

  bool pci_set(JSContext* cx, HandleObject self, HandleValue val, MutableHandleValue rval) {
    if (!ensure_override(cx, self, "PCI"))
      return false;

    if (val.isUndefined()) {
      JS::SetReservedSlot(self, Slots::PCI, val);
    } else {
      bool pci = JS::ToBoolean(val);
      set_pci(self, pci);
    }
    rval.set(CacheOverride::pci(self));
    return true;
  }

  template<auto accessor_fn>
  bool accessor_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return accessor_fn(cx, self, args.rval());
  }

  template<auto accessor_fn>
  bool accessor_set(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)
    return accessor_fn(cx, self, args[0], args.rval());
  }

  const unsigned ctor_length = 1;

  JSObject* create(JSContext* cx);

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "CacheOverride", 1))
      return false;

    RootedObject self(cx, create(cx));
    if (!self) return false;

    RootedValue val(cx);
    if (!mode_set(cx, self, args[0], &val))
      return false;

    if (mode(self) == Mode::Override) {
      if (!args.get(1).isObject()) {
        JS_ReportErrorUTF8(cx, "Creating a CacheOverride object with mode \"override\" requires "
                               "an init object for the override parameters as the second argument");
        return false;
      }

      RootedObject override_init(cx, &args[1].toObject());

      if (!JS_GetProperty(cx, override_init, "ttl", &val) ||
          !ttl_set(cx, self, val, &val))
      {
        return false;
      }

      if (!JS_GetProperty(cx, override_init, "swr", &val) ||
          !swr_set(cx, self, val, &val))
      {
        return false;
      }

      if (!JS_GetProperty(cx, override_init, "surrogateKey", &val) ||
          !surrogate_key_set(cx, self, val, &val))
      {
        return false;
      }

      if (!JS_GetProperty(cx, override_init, "pci", &val) ||
          !pci_set(cx, self, val, &val))
      {
        return false;
      }
    }

    args.rval().setObject(*self);
    return true;
  }

  const JSFunctionSpec methods[] = {JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSGS("mode", accessor_get<mode_get>, accessor_set<mode_set>, JSPROP_ENUMERATE),
    JS_PSGS("ttl", accessor_get<ttl_get>, accessor_set<ttl_set>, JSPROP_ENUMERATE),
    JS_PSGS("swr", accessor_get<swr_get>, accessor_set<swr_set>, JSPROP_ENUMERATE),
    JS_PSGS("surrogateKey", accessor_get<surrogate_key_get>, accessor_set<surrogate_key_set>, JSPROP_ENUMERATE),
    JS_PSGS("pci", accessor_get<pci_get>, accessor_set<pci_set>, JSPROP_ENUMERATE),
  JS_PS_END};

  CLASS_BOILERPLATE(CacheOverride)

  JSObject* create(JSContext* cx) {
    return JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  }
}


// https://fetch.spec.whatwg.org/#concept-method-normalize
// Returns `true` if the method name was normalized, `false` otherwise.
static bool normalize_http_method(char* method) {
  static const char* names[6] = { "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT" };

  for (size_t i = 0; i < 6; i++) {
    auto name = names[i];
    if (strcasecmp(method, name) == 0) {
      if (strcmp(method, name) == 0) {
        return false;
      }

      // Note: Safe because `strcasecmp` returning 0 above guarantees same-length strings.
      strcpy(method, name);
      return true;
    }
  }

  return false;
}

namespace Request {
  namespace Slots { enum {
    Request = RequestOrResponse::Slots::RequestOrResponse,
    Body = RequestOrResponse::Slots::Body,
    BodyStream = RequestOrResponse::Slots::BodyStream,
    HasBody = RequestOrResponse::Slots::HasBody,
    BodyUsed = RequestOrResponse::Slots::BodyUsed,
    Headers = RequestOrResponse::Slots::Headers,
    URL = RequestOrResponse::Slots::URL,
    Backend = RequestOrResponse::Slots::Count,
    Method,
    PendingRequest,
    ResponsePromise,
    IsDownstream,
    Count
  };};

  RequestHandle request_handle(JSObject* obj) {
    return RequestHandle { static_cast<uint32_t>(JS::GetReservedSlot(obj, Slots::Request).toInt32()) };
  }

  PendingRequestHandle pending_handle(JSObject* obj) {
    Value handle_val = JS::GetReservedSlot(obj, Slots::PendingRequest);
    if (handle_val.isInt32())
      return PendingRequestHandle { static_cast<uint32_t>(handle_val.toInt32()) };
    return PendingRequestHandle { INVALID_HANDLE };
  }

  bool is_pending(JSObject* obj) {
    return pending_handle(obj).handle != INVALID_HANDLE;
  }

  bool is_downstream(JSObject* obj) {
    return JS::GetReservedSlot(obj, Slots::IsDownstream).toBoolean();
  }

  JSObject* response_promise(JSObject* obj) {
    return &JS::GetReservedSlot(obj, Slots::ResponsePromise).toObject();
  }

  JSString* backend(JSObject* obj) {
    Value val = JS::GetReservedSlot(obj, Slots::Backend);
    return val.isString() ? val.toString() : nullptr;
  }

  // TODO: eagerly retrieve method and store client-side.
  JSString* method(JSContext*cx, HandleObject obj) {
    Value val = JS::GetReservedSlot(obj, Slots::Method);
    if (val.isNullOrUndefined()) {
      char buf[16];
      size_t num_written = 0;
      if (!HANDLE_RESULT(cx, xqd_req_method_get(request_handle(obj), buf, 16, &num_written)))
        return nullptr;

      RootedString method(cx, JS_NewStringCopyN(cx, buf, num_written));
      if (!method) return nullptr;

      val = JS::StringValue(method);
      JS_SetReservedSlot(obj, Slots::Method, val);
    }
    return val.toString();
  }

  bool set_cache_override(JSContext* cx, HandleObject self, HandleValue cache_override_val) {
    if (!CacheOverride::is_instance(cache_override_val)) {
      JS_ReportErrorUTF8(cx, "Value passed in as cacheOverride must be an "
                             "instance of CacheOverride");
      return false;
    }

    RootedObject cache_override(cx, &cache_override_val.toObject());
    RootedValue val(cx);

    uint32_t tag = CacheOverride::abi_tag(cache_override);
    val = CacheOverride::ttl(cache_override);
    uint32_t ttl = val.isUndefined() ? 0 : val.toInt32();
    val = CacheOverride::swr(cache_override);
    uint32_t swr = val.isUndefined() ? 0 : val.toInt32();
    val = CacheOverride::surrogate_key(cache_override);
    UniqueChars sk_chars;
    size_t sk_len = 0;
    if (!val.isUndefined()) {
      sk_chars = encode(cx, val, &sk_len);
      if (!sk_chars) return false;
    }

    return HANDLE_RESULT(cx, xqd_req_cache_override_v2_set(request_handle(self), tag, ttl, swr,
                                                           sk_chars.get(), sk_len));
  }

  JSObject* create(JSContext* cx, HandleValue input, HandleValue init);

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    REQUEST_HANDLER_ONLY("The Request builtin");
    if (!args.requireAtLeast(cx, "Request", 1))
      return false;

    RootedObject request(cx, create(cx, args[0], args.get(1)));
    if (!request) return false;

    args.rval().setObject(*request);
    return true;
  }

  const unsigned ctor_length = 1;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool method_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    JSString* method = Request::method(cx, self);
    if (!method)
      return false;

    args.rval().setString(method);
    return true;
  }

  bool url_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    args.rval().set(RequestOrResponse::url(self));
    return true;
  }

  bool version_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    uint32_t version = 0;
    if (!HANDLE_RESULT(cx, xqd_req_version_get(request_handle(self), &version)))
      return false;

    args.rval().setInt32(version);
    return true;
  }

  bool headers_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    JSObject* headers = RequestOrResponse::headers<Headers::Mode::ProxyToRequest>(cx, self);
    if (!headers)
      return false;

    args.rval().setObject(*headers);
    return true;
  }

  template<BodyReadResult result_type>
  bool bodyAll(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return RequestOrResponse::bodyAll<result_type>(cx, args, self);
  }

  bool body_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return RequestOrResponse::body_get(cx, args, self, is_downstream(self));
  }

  bool bodyUsed_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    args.rval().setBoolean(RequestOrResponse::body_used(self));
    return true;
  }

  bool setCacheOverride(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    if (!set_cache_override(cx, self, args[0]))
      return false;

    args.rval().setUndefined();
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("arrayBuffer", bodyAll<BodyReadResult::ArrayBuffer>, 0, 0),
    JS_FN("json", bodyAll<BodyReadResult::JSON>, 0, 0),
    JS_FN("text", bodyAll<BodyReadResult::Text>, 0, 0),
    JS_FN("setCacheOverride", setCacheOverride, 3, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("method", method_get, 0),
    JS_PSG("url", url_get, 0),
    JS_PSG("version", version_get, 0),
    JS_PSG("headers", headers_get, 0),
    JS_PSG("body", body_get, 0),
    JS_PSG("bodyUsed", bodyUsed_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE(Request)

  JSObject* create(JSContext* cx, RequestHandle request_handle, BodyHandle body_handle, bool is_downstream) {
    RootedObject request(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!request) return nullptr;

    JS::SetReservedSlot(request, Slots::Request, JS::Int32Value(request_handle.handle));
    JS::SetReservedSlot(request, Slots::Body, JS::Int32Value(body_handle.handle));
    // TODO: properly set HasBody based on whether we actually do have a body.
    JS::SetReservedSlot(request, Slots::HasBody, JS::BooleanValue(true));
    JS::SetReservedSlot(request, Slots::BodyUsed, JS::BooleanValue(false));
    JS::SetReservedSlot(request, Slots::IsDownstream, JS::BooleanValue(is_downstream));

    return request;
  }

  JSObject* create(JSContext* cx, HandleValue input, HandleValue init_val) {
    RequestHandle request_handle = { INVALID_HANDLE };
    BodyHandle body_handle = { INVALID_HANDLE };
    RootedString url_str(cx);

    // TODO: clone the handles if needed, e.g. if the new request is sent
    // multiple times, or its headers are changed, etc.
    // This is costly, as it requires copying all the headers, etc, and
    // probably also the body, so we should only do it if really necessary.
    if (is_instance(input)) {
      RootedObject input_request(cx, &input.toObject());
      request_handle = Request::request_handle(input_request);
      body_handle = RequestOrResponse::body_handle(input_request);
      url_str = RequestOrResponse::url(input_request).toString();
      if (is_downstream(input_request)) {
        RootedObject headers(cx);
        headers = RequestOrResponse::headers<Headers::Mode::ProxyToRequest>(cx, input_request);
        if (!Headers::delazify(cx, headers))
          return nullptr;
      }
    } else {
      RootedObject url_obj(cx, URL::create(cx, input, Fastly::baseURL));
      if (!url_obj) return nullptr;

      RootedValue url_val(cx, JS::ObjectValue(*url_obj));
      url_str = JS::ToString(cx, url_val);
      if (!url_str) return nullptr;

      // TODO: get the SpecSlice directly here instead of re-encoding.
      size_t url_len;
      UniqueChars url = encode(cx, url_str, &url_len);
      if (!url) return nullptr;

      if (!(HANDLE_RESULT(cx, xqd_req_new(&request_handle)) &&
            HANDLE_RESULT(cx, xqd_req_uri_set(request_handle, url.get(), url_len)) &&
            HANDLE_RESULT(cx, xqd_body_new(&body_handle))))
      {
        return nullptr;
      }
    }

    RootedObject request(cx, create(cx, request_handle, body_handle, false));
    if (!request) return nullptr;

    RequestOrResponse::set_url(request, StringValue(url_str));

    // TODO: apply cache and referrer from init object.

    if (init_val.isObject()) {
      RootedObject init(cx, &init_val.toObject());

      RootedValue method_val(cx);
      if (!JS_GetProperty(cx, init, "method", &method_val))
        return nullptr;

      UniqueChars method;
      if (!method_val.isUndefined()) {
        RootedString method_str(cx, JS::ToString(cx, method_val));
        if (!method_str) return nullptr;

        size_t method_len;
        method = encode(cx, method_str, &method_len);
        if (!method) return nullptr;

        if (normalize_http_method(method.get())) {
          // Replace the JS string with the normalized name.
          method_str = JS_NewStringCopyN(cx, method.get(), method_len);
          if (!method_str) return nullptr;
        }

        if (!HANDLE_RESULT(cx, xqd_req_method_set(request_handle, method.get(), method_len)))
          return nullptr;
        JS::SetReservedSlot(request, Slots::Method, JS::StringValue(method_str));
      }

      RootedValue body_val(cx);
      if (!JS_GetProperty(cx, init, "body", &body_val))
        return nullptr;

      if (!body_val.isNullOrUndefined()) {
        // TODO: throw if `method` is "GET" or "HEAD"
        if (!RequestOrResponse::set_body(cx, request, body_val))
          return nullptr;
      }

      RootedValue headers_val(cx);
      if (!JS_GetProperty(cx, init, "headers", &headers_val))
        return nullptr;

      if (!headers_val.isUndefined()) {
        RootedObject headers(cx, Headers::create(cx, Headers::Mode::ProxyToRequest,
                                                 request, headers_val));
        if (!headers) return nullptr;
        JS::SetReservedSlot(request, Slots::Headers, JS::ObjectValue(*headers));
      }

      RootedValue backend_val(cx);
      if (!JS_GetProperty(cx, init, "backend", &backend_val))
        return nullptr;
      if (!backend_val.isUndefined()) {
        RootedString backend(cx, JS::ToString(cx, backend_val));
        if (!backend) return nullptr;
        JS::SetReservedSlot(request, Slots::Backend, JS::StringValue(backend));
      }

      RootedValue cache_override(cx);
      if (!JS_GetProperty(cx, init, "cacheOverride", &cache_override))
        return nullptr;
      if (!cache_override.isUndefined() && !set_cache_override(cx, request, cache_override))
          return nullptr;
    }

    return request;
  }
}


namespace Response {
  namespace Slots { enum {
    Response = RequestOrResponse::Slots::RequestOrResponse,
    Body = RequestOrResponse::Slots::Body,
    BodyStream = RequestOrResponse::Slots::BodyStream,
    HasBody = RequestOrResponse::Slots::HasBody,
    BodyUsed = RequestOrResponse::Slots::BodyUsed,
    Headers = RequestOrResponse::Slots::Headers,
    IsUpstream = RequestOrResponse::Slots::Count,
    Status,
    Count
  };};

  // Needed for uniform access to Request and Response slots.
  static_assert((int)Slots::Body == (int)Request::Slots::Body);
  static_assert((int)Slots::BodyStream == (int)Request::Slots::BodyStream);
  static_assert((int)Slots::HasBody == (int)Request::Slots::HasBody);
  static_assert((int)Slots::BodyUsed == (int)Request::Slots::BodyUsed);
  static_assert((int)Slots::Headers == (int)Request::Slots::Headers);
  static_assert((int)Slots::Response == (int)Request::Slots::Request);

  ResponseHandle response_handle(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return ResponseHandle { (uint32_t)(JS::GetReservedSlot(obj, Slots::Response).toInt32()) };
  }

  bool is_upstream(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return JS::GetReservedSlot(obj, Slots::IsUpstream).toBoolean();
  }

  uint16_t status(JSObject* obj) {
    MOZ_ASSERT(is_instance(obj));
    return (uint16_t)JS::GetReservedSlot(obj, Slots::Status).toInt32();
  }

  JSObject* create(JSContext* cx, ResponseHandle response_handle, BodyHandle body_handle,
                   bool is_upstream);

  // TODO: consider not creating a host-side representation for responses eagerly.
  // Some applications create Response objects purely for internal use, e.g. to represent
  // cache entries. While that's perhaps not ideal to begin with, it exists, so we should
  // handle it in a good way, and not be superfluously slow.
  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    REQUEST_HANDLER_ONLY("The Response builtin");

    // TODO: enable creating Response objects during the init phase, and only
    // creating the host-side representation when processing requests.
    ResponseHandle response_handle = { .handle = INVALID_HANDLE };
    if (!HANDLE_RESULT(cx, xqd_resp_new(&response_handle)))
      return false;

    BodyHandle body_handle = { .handle = INVALID_HANDLE };
    if (!HANDLE_RESULT(cx, xqd_body_new(&body_handle)))
      return false;

    RootedObject response(cx, create(cx, response_handle, body_handle, false));
    if (!response) return false;

    if (!RequestOrResponse::set_body(cx, response, args.get(0)))
      return false;

    RequestOrResponse::set_url(response, JS::StringValue(JS_GetEmptyString(cx)));

    if (args.get(1).isObject()) {
      RootedObject init(cx, &args[1].toObject());

      RootedValue status_val(cx);
      if (!JS_GetProperty(cx, init, "status", &status_val))
        return false;

      uint16_t status = 200;
      if (!status_val.isUndefined() && !JS::ToUint16(cx, status_val, &status))
        return false;

      if (!HANDLE_RESULT(cx, xqd_resp_status_set(response_handle, status)))
        return false;

      RootedValue headers_val(cx);
      if (!JS_GetProperty(cx, init, "headers", &headers_val))
        return false;

      if (!headers_val.isUndefined()) {
        RootedObject headers(cx, Headers::create(cx, Headers::Mode::ProxyToResponse,
                                                 response, headers_val));
        if (!headers) return false;

        JS::SetReservedSlot(response, Slots::Headers, JS::ObjectValue(*headers));
      }
    }

    // To ensure that we really have the same status value as the host,
    // we always read it back here.
    uint16_t status = 0;
    if (!HANDLE_RESULT(cx, xqd_resp_status_get(response_handle, &status)))
      return false;

    JS::SetReservedSlot(response, Slots::Status, JS::Int32Value(status));
    args.rval().setObject(*response);
    return true;
  }

  const unsigned ctor_length = 1;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool ok_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    uint16_t status = Response::status(self);
    args.rval().setBoolean(status >= 200 && status < 300);
    return true;
  }

  bool status_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    uint16_t status = Response::status(self);
    args.rval().setInt32(status);
    return true;
  }

  bool url_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    args.rval().set(RequestOrResponse::url(self));
    return true;
  }

  // TODO: store version client-side.
  bool version_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    uint32_t version = 0;
    if (!HANDLE_RESULT(cx, xqd_resp_version_get(response_handle(self), &version)))
      return false;

    args.rval().setInt32(version);
    return true;
  }

  bool headers_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    JSObject* headers = RequestOrResponse::headers<Headers::Mode::ProxyToResponse>(cx, self);
    if (!headers)
      return false;

    args.rval().setObject(*headers);
    return true;
  }

  template<BodyReadResult result_type>
  bool bodyAll(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return RequestOrResponse::bodyAll<result_type>(cx, args, self);
  }

  bool body_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return RequestOrResponse::body_get(cx, args, self, true);
  }

  bool bodyUsed_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    args.rval().setBoolean(RequestOrResponse::body_used(self));
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("arrayBuffer", bodyAll<BodyReadResult::ArrayBuffer>, 0, 0),
    JS_FN("json", bodyAll<BodyReadResult::JSON>, 0, 0),
    JS_FN("text", bodyAll<BodyReadResult::Text>, 0, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("ok", ok_get, 0),
    JS_PSG("status", status_get, 0),
    JS_PSG("version", version_get, 0),
    JS_PSG("headers", headers_get, 0),
    JS_PSG("body", body_get, 0),
    JS_PSG("bodyUsed", bodyUsed_get, 0),
    JS_PSG("url", url_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE(Response)

  JSObject* create(JSContext* cx, ResponseHandle response_handle, BodyHandle body_handle,
                   bool is_upstream)
  {
    RootedObject response(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!response) return nullptr;

    JS::SetReservedSlot(response, Slots::Response, JS::Int32Value(response_handle.handle));
    JS::SetReservedSlot(response, Slots::Body, JS::Int32Value(body_handle.handle));
    // TODO: only set HasBody for upstream responses if we actually do have a body.
    JS::SetReservedSlot(response, Slots::HasBody, JS::BooleanValue(is_upstream));
    JS::SetReservedSlot(response, Slots::BodyUsed, JS::BooleanValue(false));
    JS::SetReservedSlot(response, Slots::IsUpstream, JS::BooleanValue(is_upstream));

    if (is_upstream) {
      uint16_t status = 0;
      if (!HANDLE_RESULT(cx, xqd_resp_status_get(response_handle, &status)))
        return nullptr;

      JS::SetReservedSlot(response, Slots::Status, JS::Int32Value(status));
    }

    return response;
  }
}


namespace Dictionary {
  namespace Slots { enum {
    Dictionary,
    Count
  };};

  DictionaryHandle dictionary_handle(JSObject* obj) {
    JS::Value val = JS::GetReservedSlot(obj, Slots::Dictionary);
    return DictionaryHandle { static_cast<uint32_t>(val.toInt32()) };
  }

  JSObject* create(JSContext* cx, const char* name, size_t name_len);

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    REQUEST_HANDLER_ONLY("The Dictionary builtin");
    if (!args.requireAtLeast(cx, "Dictionary", 1))
      return false;

    size_t name_len;
    UniqueChars name = encode(cx, args[0], &name_len);
    RootedObject dictionary(cx, create(cx, name.get(), name_len));
    if (!dictionary) return false;
    args.rval().setObject(*dictionary);
    return true;
  }

  const unsigned ctor_length = 1;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    size_t name_len;
    UniqueChars name = encode(cx, args[0], &name_len);

    OwnedHostCallBuffer buffer;
    size_t nwritten = 0;
    int status = xqd_dictionary_get(dictionary_handle(self), name.get(), name_len, buffer.get(),
                                    DICTIONARY_ENTRY_MAX_LEN, &nwritten);
    // Status code 10 indicates the key wasn't found, so we return null.
    if (status == 10) {
      args.rval().setNull();
      return true;
    }

    // Ensure that we throw an exception for all unexpected host errors.
    if (!HANDLE_RESULT(cx, status))
      return false;

    RootedString text(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(buffer.get(), nwritten)));
    if (!text) return false;

    args.rval().setString(text);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("get", get, 1, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE(Dictionary)

  JSObject* create(JSContext* cx, const char* name, size_t name_len) {
    RootedObject dict(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!dict) return nullptr;
    DictionaryHandle dict_handle = { INVALID_HANDLE };
    if (!HANDLE_RESULT(cx, xqd_dictionary_open(name, name_len, &dict_handle)))
      return nullptr;

    JS::SetReservedSlot(dict, Slots::Dictionary, JS::Int32Value((int)dict_handle.handle));

    return dict;
  }
}

namespace TextEncoder {
  namespace Slots { enum {
    Count
  };};

  JSObject* create(JSContext* cx);

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject self(cx, create(cx));
    if (!self) return false;

    args.rval().setObject(*self);
    return true;
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool encode(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    // Default to empty string if no input is given.
    if (args.get(0).isUndefined()) {
      RootedObject byte_array(cx, JS_NewUint8Array(cx, 0));
      if (!byte_array)
        return false;

      args.rval().setObject(*byte_array);
      return true;
    }

    size_t chars_len;
    UniqueChars chars = encode(cx, args[0], &chars_len);

    auto* rawChars = chars.release();
    RootedObject buffer(cx, JS::NewArrayBufferWithContents(cx, chars_len, rawChars));
    if (!buffer) {
      JS_free(cx, rawChars);
      return false;
    }

    RootedObject byte_array(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, chars_len));
    if (!byte_array) return false;

    args.rval().setObject(*byte_array);
    return true;
  }

  bool encoding_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedString str(cx, JS_NewStringCopyN(cx, "utf-8", 5));
    if (!str)
      return false;

    args.rval().setString(str);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("encode", encode, 1, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("encoding", encoding_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE(TextEncoder)

  JSObject* create(JSContext* cx) {
    return JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  }
}

namespace TextDecoder {
  namespace Slots { enum {
    Count
  };};

  JSObject* create(JSContext* cx);

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject self(cx, create(cx));
    if (!self) return false;

    args.rval().setObject(*self);
    return true;
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool decode(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    // Default to empty string if no input is given.
    if (args.get(0).isUndefined()) {
      args.rval().set(JS_GetEmptyStringValue(cx));
      return true;
    }

    if (!args[0].isObject() || !(JS_IsArrayBufferViewObject(&args[0].toObject()) ||
                                 JS::IsArrayBufferObject(&args[0].toObject())) )
    {
      JS_ReportErrorUTF8(cx, "TextDecoder#decode: input must be of type ArrayBuffer or ArrayBufferView");
      return false;
    }

    RootedObject input(cx, &args[0].toObject());
    size_t length;
    uint8_t* data;
    bool is_shared;

    if (JS_IsArrayBufferViewObject(input)) {
      js::GetArrayBufferViewLengthAndData(input, &length, &is_shared, &data);
    } else {
      JS::GetArrayBufferLengthAndData(input, &length, &is_shared, &data);
    }

    RootedString str(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)data, length)));
    if (!str)
      return false;

    args.rval().setString(str);
    return true;
  }

  bool encoding_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedString str(cx, JS_NewStringCopyN(cx, "utf-8", 5));
    if (!str)
      return false;

    args.rval().setString(str);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("decode", decode, 1, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("encoding", encoding_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE(TextDecoder)

  JSObject* create(JSContext* cx) {
    return JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  }
}


bool report_sequence_or_record_arg_error(JSContext* cx, const char* name, const char* alt_text) {
  JS_ReportErrorUTF8(cx, "Failed to construct %s object. If defined, the first "
                          "argument must be either a [ ['name', 'value'], ... ] sequence, "
                          "or a { 'name' : 'value', ... } record%s.", name, alt_text);
  return false;
}
/**
 * Extract <key,value> pairs from the given value if it is either a sequence<sequence<Value>
 * or a record<Value, Value>.
 */
template<auto apply>
bool maybe_consume_sequence_or_record(JSContext* cx, HandleValue initv, HandleObject target,
                                      bool* consumed, const char* ctor_name,
                                      const char* alt_text = "")
{
    if (initv.isUndefined()) {
      *consumed = true;
      return true;
    }

    RootedValue key(cx);
    RootedValue value(cx);

    // First, try consuming args[0] as a sequence<sequence<Value>>.
    JS::ForOfIterator it(cx);
    if (!it.init(initv, JS::ForOfIterator::AllowNonIterable))
      return false;

    // Note: this currently doesn't treat strings as iterable even though they are.
    // We don't have any constructors that want to iterate over strings, and this
    // makes things a lot easier.
    if (initv.isObject() && it.valueIsIterable()) {
      RootedValue entry(cx);

      while (true) {
        bool done;
        if (!it.next(&entry, &done))
          return false;

        if (done)
          break;

        if (!entry.isObject())
          return report_sequence_or_record_arg_error(cx, ctor_name, alt_text);

        JS::ForOfIterator entr_iter(cx);
        if (!entr_iter.init(entry, JS::ForOfIterator::AllowNonIterable))
          return false;

        if (!entr_iter.valueIsIterable())
          return report_sequence_or_record_arg_error(cx, ctor_name, alt_text);

        {
          bool done;

          // Extract key.
          if (!entr_iter.next(&key, &done))
            return false;
          if (done)
            return report_sequence_or_record_arg_error(cx, ctor_name, alt_text);

          // Extract value.
          if (!entr_iter.next(&value, &done))
            return false;
          if (done)
            return report_sequence_or_record_arg_error(cx, ctor_name, alt_text);

          // Ensure that there aren't any further entries.
          if (!entr_iter.next(&entry, &done))
            return false;
          if (!done)
            return report_sequence_or_record_arg_error(cx, ctor_name, alt_text);

          if (!apply(cx, target, key, value, ctor_name))
            return false;
        }
      }
      *consumed = true;
    } else if (initv.isObject()) {
      // init isn't an iterator, so if it's an object, it must be a record to be valid input.
      RootedObject init(cx, &initv.toObject());
      JS::RootedIdVector ids(cx);
      if (!js::GetPropertyKeys(cx, init, JSITER_OWNONLY | JSITER_SYMBOLS, &ids))
        return false;

      JS::RootedId curId(cx);
      for (size_t i = 0; i < ids.length(); ++i) {
        curId = ids[i];
        key = js::IdToValue(curId);

        if (!JS_GetPropertyById(cx, init, curId, &value))
          return false;

        if (!apply(cx, target, key, value, ctor_name))
          return false;
      }
      *consumed = true;
    } else {
      *consumed = false;
    }

    return true;
}
namespace Headers {
  namespace Slots { enum {
    BackingMap,
    Handle,
    Mode,
    HasLazyValues,
    Count
  };};

  namespace detail {
    #define HEADERS_ITERATION_METHOD(argc) \
      METHOD_HEADER(argc) \
      RootedObject backing_map(cx, detail::backing_map(self)); \
      if (!detail::ensure_all_header_values_from_handle(cx, self, backing_map)) \
        return false; \

    static const char VALID_NAME_CHARS[128] = {
      0, 0, 0, 0, 0, 0, 0, 0,  //   0
      0, 0, 0, 0, 0, 0, 0, 0,  //   8
      0, 0, 0, 0, 0, 0, 0, 0,  //  16
      0, 0, 0, 0, 0, 0, 0, 0,  //  24

      0, 1, 0, 1, 1, 1, 1, 1,  //  32
      0, 0, 1, 1, 0, 1, 1, 0,  //  40
      1, 1, 1, 1, 1, 1, 1, 1,  //  48
      1, 1, 0, 0, 0, 0, 0, 0,  //  56

      0, 1, 1, 1, 1, 1, 1, 1,  //  64
      1, 1, 1, 1, 1, 1, 1, 1,  //  72
      1, 1, 1, 1, 1, 1, 1, 1,  //  80
      1, 1, 1, 0, 0, 0, 1, 1,  //  88

      1, 1, 1, 1, 1, 1, 1, 1,  //  96
      1, 1, 1, 1, 1, 1, 1, 1,  // 104
      1, 1, 1, 1, 1, 1, 1, 1,  // 112
      1, 1, 1, 0, 1, 0, 1, 0   // 120
    };

    #define NORMALIZE_NAME(name, fun_name) \
      RootedValue normalized_name(cx, name); \
      size_t name_len; \
      UniqueChars name_chars = detail::normalize_header_name(cx, &normalized_name, &name_len, \
                                                             fun_name); \
      if (!name_chars) return false; \

    #define NORMALIZE_VALUE(value, fun_name) \
      RootedValue normalized_value(cx, value); \
      size_t value_len; \
      UniqueChars value_chars = detail::normalize_header_value(cx, &normalized_value, \
                                                               &value_len, fun_name); \
      if (!value_chars) return false; \

    JSObject* backing_map(JSObject* self) {
      return &JS::GetReservedSlot(self, Slots::BackingMap).toObject();
    }

    Mode mode(JSObject* self) {
      return static_cast<Mode>(JS::GetReservedSlot(self, Slots::Mode).toInt32());
    }

    bool lazy_values(JSObject* self) {
      return JS::GetReservedSlot(self, Slots::HasLazyValues).toBoolean();
    }

    uint32_t handle(JSObject* self) {
      return static_cast<uint32_t>(JS::GetReservedSlot(self, Slots::Handle).toInt32());
    }

    /**
     * Validates and normalizes the given header name, by
     * - checking for invalid characters
     * - converting to lower-case
     *
     * See https://searchfox.org/mozilla-central/rev/9f76a47f4aa935b49754c5608a1c8e72ee358c46/netwerk/protocol/http/nsHttp.cpp#172-215
     * For details on validation.
     *
     * Mutates `name_val` in place, and returns the name as UniqueChars.
     * This is done because most uses of header names require handling of both the JSString
     * and the char* version, so they'd otherwise have to recreate one of the two.
     */
    UniqueChars normalize_header_name(JSContext* cx, MutableHandleValue name_val,
                                      size_t* name_len, const char* fun_name)
    {
      RootedString name_str(cx, JS::ToString(cx, name_val));
      if (!name_str) return nullptr;

      size_t len;
      UniqueChars name = encode(cx, name_str, &len);
      if (!name) return nullptr;

      if (len == 0) {
        JS_ReportErrorASCII(cx, "%s: Header name can't be empty", fun_name);
        return nullptr;
      }

      bool changed = false;

      char* name_chars = name.get();
      for (size_t i = 0; i < len; i++) {
        unsigned char ch = name_chars[i];
        if (ch > 127 || !VALID_NAME_CHARS[ch]) {
          JS_ReportErrorASCII(cx, "%s: Invalid header name '%s'", fun_name, name_chars);
          return nullptr;
        }

        if (ch >= 'A' && ch <= 'Z') {
          name_chars[i] = ch - 'A' + 'a';
          changed = true;
        }
      }

      if (changed) {
        name_str = JS_NewStringCopyN(cx, name_chars, len);
        if (!name_str)
          return nullptr;
      }

      name_val.setString(name_str);
      *name_len = len;
      return name;
    }

    UniqueChars normalize_header_value(JSContext* cx, MutableHandleValue value_val,
                                       size_t* value_len, const char* fun_name)
    {
      RootedString value_str(cx, JS::ToString(cx, value_val));
      if (!value_str) return nullptr;

      size_t len;
      UniqueChars value = encode(cx, value_str, &len);
      if (!value) return nullptr;

      char* value_chars = value.get();
      size_t start = 0;
      size_t end = len;

      // We follow Gecko's interpretation of what's a valid header value. After stripping
      // leading and trailing whitespace, all interior line breaks and `\0` are considered
      // invalid.
      // See https://searchfox.org/mozilla-central/rev/9f76a47f4aa935b49754c5608a1c8e72ee358c46/netwerk/protocol/http/nsHttp.cpp#247-260
      // for details.
      while (start < end) {
        unsigned char ch = value_chars[start];
        if (ch == '\t' || ch == ' ' || ch == '\r' || ch == '\n') {
            start++;
        } else {
            break;
        }
      }

      while (end > start) {
        unsigned char ch = value_chars[end - 1];
        if (ch == '\t' || ch == ' ' || ch == '\r' || ch == '\n') {
              end--;
          } else {
              break;
          }
      }

      for (size_t i = start; i < end; i++) {
        unsigned char ch = value_chars[i];
        if (ch == '\r' || ch == '\n' || ch == '\0') {
          JS_ReportErrorUTF8(cx, "%s: Invalid header value '%s'", fun_name, value_chars);
          return nullptr;
        }
      }

      if (start != 0 || end != len) {
        value_str = JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(value_chars + start, end - start));
        if (!value_str) return nullptr;
      }

      value_val.setString(value_str);
      *value_len = len;
      return value;
    }

    static PersistentRooted<JSString*> comma;

    // Append an already normalized value for an already normalized header name
    // to the JS side map, but not the host.
    //
    // Returns the resulting combined value in `value`.
    bool append_header_value_to_map(JSContext* cx, HandleObject self,
                                    HandleValue normalized_name,
                                    MutableHandleValue normalized_value)
    {
      RootedValue existing(cx);
      RootedObject map(cx, backing_map(self));
      if (!JS::MapGet(cx, map, normalized_name, &existing))
        return false;

      // Existing value must only be null if we're in the process if applying
      // header values from a handle.
      if (!existing.isNullOrUndefined()) {
        if (!comma.get()) {
          comma.init(cx, JS_NewStringCopyN(cx, ", ", 2));
          if (!comma) return false;
        }

        RootedString str(cx, existing.toString());
        str = JS_ConcatStrings(cx, str, comma);
        if (!str) return false;

        RootedString val_str(cx, normalized_value.toString());
        str = JS_ConcatStrings(cx, str, val_str);
        if (!str) return false;

        normalized_value.setString(str);
      }

      return JS::MapSet(cx, map, normalized_name, normalized_value);
    }

    bool get_header_names_from_handle(JSContext* cx, uint32_t handle, Mode mode,
                                      HandleObject backing_map)
    {
      RootedString name(cx);
      RootedValue name_val(cx);
      OwnedHostCallBuffer buffer;

      MULTI_VALUE_HOSTCALL({
        int result;
        if (mode == Mode::ProxyToRequest) {
          RequestHandle request = { handle };
          result = xqd_req_header_names_get(request, buffer.get(), HEADER_MAX_LEN,
                                            cursor, &ending_cursor,
                                            &nwritten);
        } else {
          ResponseHandle response = { handle };
          result = xqd_resp_header_names_get(response, buffer.get(), HEADER_MAX_LEN,
                                             cursor, &ending_cursor,
                                             &nwritten);
        }

        if (!HANDLE_RESULT(cx, result))
          return false;
      },
      {
        uint32_t offset = 0;
        for (size_t i = 0; i < nwritten; i++)
        {
          if (buffer.get()[i] != '\0') {
            continue;
          }

          name = JS_NewStringCopyN(cx, buffer.get() + offset, i - offset);
          if (!name) return false;

          name_val.setString(name);
          JS::MapSet(cx, backing_map, name_val, JS::NullHandleValue);

          offset = i + 1;
        }
      })

      return true;
    }

    static bool retrieve_value_for_header_from_handle(JSContext* cx, HandleObject self,
                                                      HandleValue name, MutableHandleValue value)
    {
      Mode mode = detail::mode(self);
      MOZ_ASSERT(mode != Mode::Standalone);
      uint32_t handle = detail::handle(self);

      size_t name_len;
      RootedString name_str(cx, name.toString());
      UniqueChars name_chars = encode(cx, name_str, &name_len);

      RootedString val_str(cx);
      OwnedHostCallBuffer buffer;

      MULTI_VALUE_HOSTCALL({
        int result;
        if (mode == Headers::Mode::ProxyToRequest) {
          RequestHandle request = { handle };
          result = xqd_req_header_values_get(request, name_chars.get(), name_len,
                                             buffer.get(), HEADER_MAX_LEN,
                                             cursor, &ending_cursor, &nwritten);
        } else {
          ResponseHandle response = { handle };
              result = xqd_resp_header_values_get(response, name_chars.get(), name_len,
                                                  buffer.get(), HEADER_MAX_LEN,
                                                  cursor, &ending_cursor, &nwritten);
        }

        if (!HANDLE_RESULT(cx, result))
          return false;
      }, {
        size_t offset = 0;
        for (size_t i = 0; i < nwritten; i++) {
          if (buffer.get()[i] == '\0') {
            val_str = JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(buffer.get() + offset,
                                                              i - offset));
            if (!val_str) return false;

            value.setString(val_str);

            if (!append_header_value_to_map(cx, self, name, value))
              return false;

            offset = i + 1;
          }
        }
      })

      return true;
    }

    /**
     * Ensures that a value for the given header is available to client code.
     *
     * The calling code must ensure that a header with the given name exists, but might not yet
     * have been retrieved from the host, i.e., it might be a "lazy" value.
     *
     * The value is returned via the `values` outparam, but *only* if the Headers object has
     * lazy values at all. This is to avoid the map lookup in those cases where none is
     * necessary in this function, and the consumer wouldn't use the value anyway.
     */
    static bool ensure_value_for_header(JSContext* cx, HandleObject self,
                                        HandleValue normalized_name, MutableHandleValue values)
    {
      if (!detail::lazy_values(self))
        return true;

      RootedObject map(cx, detail::backing_map(self));
      if (!JS::MapGet(cx, map, normalized_name, values))
        return false;

      // Value isn't lazy, just return it.
      if (!values.isNull())
        return true;

      return detail::retrieve_value_for_header_from_handle(cx, self, normalized_name, values);
    }

    bool get_header_value_for_name(JSContext* cx, HandleObject self, HandleValue name,
                                   MutableHandleValue rval, const char* fun_name)
    {
      NORMALIZE_NAME(name, fun_name)

      if (!ensure_value_for_header(cx, self, normalized_name, rval))
        return false;

      if (rval.isString())
        return true;

      RootedObject map(cx, detail::backing_map(self));
      return JS::MapGet(cx, map, normalized_name, rval);
    }

    static bool ensure_all_header_values_from_handle(JSContext* cx, HandleObject self,
                                                     HandleObject backing_map)
    {
      if (!lazy_values(self))
        return true;

      RootedValue iterable(cx);
      if (!JS::MapKeys(cx, backing_map, &iterable))
        return false;

      JS::ForOfIterator it(cx);
      if (!it.init(iterable))
        return false;

      RootedValue name(cx);
      RootedValue v(cx);
      while (true) {
        bool done;
        if (!it.next(&name, &done))
          return false;

        if (done)
          break;

        if (!ensure_value_for_header(cx, self, name, &v))
          return false;
      }

      JS::SetReservedSlot(self, Slots::HasLazyValues, JS::BooleanValue(false));

      return true;
    }

    typedef int AppendHeaderOperation(int handle, const char *name, size_t name_len,
                                      const char *value, size_t value_len);

    // Appends a non-normalized value for a non-normalized header name to both
    // the JS side Map and, in non-standalone mode, the host.
    //
    // Verifies and normalizes the name and value.
    bool append_header_value(JSContext* cx, HandleObject self,
                             HandleValue name, HandleValue value, const char* fun_name)
    {
      NORMALIZE_NAME(name, fun_name)
      NORMALIZE_VALUE(value, fun_name)

      // Ensure that any host-side values have been applied JS-side.
      RootedValue v(cx);
      if (!ensure_value_for_header(cx, self, normalized_name, &v))
        return false;

      Mode mode = detail::mode(self);
      if (mode != Mode::Standalone) {
        AppendHeaderOperation* op;
        if (mode == Mode::ProxyToRequest)
          op = (AppendHeaderOperation*)xqd_req_header_append;
        else
          op = (AppendHeaderOperation*)xqd_resp_header_append;
        if (!HANDLE_RESULT(cx, op(handle(self), name_chars.get(), name_len,
                           value_chars.get(), value_len)))
        {
          return false;
        }
      }

      return append_header_value_to_map(cx, self, normalized_name, &normalized_value);
    }
  }

  bool delazify(JSContext* cx, HandleObject headers) {
    RootedObject backing_map(cx, detail::backing_map(headers));
    return detail::ensure_all_header_values_from_handle(cx, headers, backing_map);
  }

  JSObject* create(JSContext* cx, Mode mode, HandleObject owner, HandleValue initv) {
    RootedObject headers(cx, create(cx, mode, owner));
    if (!headers) return nullptr;

    bool consumed = false;
    if (!maybe_consume_sequence_or_record<detail::append_header_value>(cx, initv, headers,
                                                                       &consumed, "Headers"))
    {
      return nullptr;
    }

    if (!consumed) {
      report_sequence_or_record_arg_error(cx, "Headers", "");
      return nullptr;
    }

    return headers;
  }

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject headers(cx, create(cx, Mode::Standalone, nullptr, args.get(0)));
    if (!headers) return false;

    args.rval().setObject(*headers);
    return true;
  }

  const unsigned ctor_length = 1;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    NORMALIZE_NAME(args[0], "Headers.get")

    return detail::get_header_value_for_name(cx, self, normalized_name, args.rval(),
                                             "Headers.get");
  }

  typedef int HeaderValuesSetOperation(int handle, const char *name, size_t name_len,
                                       const char *values, size_t values_len);

  bool set(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(2)

    NORMALIZE_NAME(args[0], "Headers.set")
    NORMALIZE_VALUE(args[1], "Headers.set")

    Mode mode = detail::mode(self);
    if (mode != Mode::Standalone) {
      HeaderValuesSetOperation* op;
      if (mode == Mode::ProxyToRequest)
        op = (HeaderValuesSetOperation*)xqd_req_header_insert;
      else
        op = (HeaderValuesSetOperation*)xqd_resp_header_insert;
      if (!HANDLE_RESULT(cx, op(detail::handle(self), name_chars.get(), name_len,
                         value_chars.get(), value_len)))
      {
        return false;
      }
    }

    RootedObject map(cx, detail::backing_map(self));
    if (!JS::MapSet(cx, map, normalized_name, normalized_value))
      return false;

    args.rval().setUndefined();
    return true;
  }

  bool has(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    NORMALIZE_NAME(args[0], "Headers.has")
    bool has;
    RootedObject map(cx, detail::backing_map(self));
    if (!JS::MapHas(cx, map, normalized_name, &has))
      return false;
    args.rval().setBoolean(has);
    return true;
  }

  bool append(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(2)

    if (!detail::append_header_value(cx, self, args[0], args[1], "Headers.append"))
      return false;

    args.rval().setUndefined();
    return true;
  }

  typedef int HeaderRemoveOperation(int handle, const char *name, size_t name_len);

  bool delete_(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(1, "delete")

    NORMALIZE_NAME(args[0], "Headers.delete")

    bool has;
    RootedObject map(cx, detail::backing_map(self));
    if (!JS::MapDelete(cx, map, normalized_name, &has))
      return false;

    // If no header with the given name exists, `delete` is a no-op.
    if (!has) {
      args.rval().setUndefined();
      return true;
    }

    Mode mode = detail::mode(self);
    if (mode != Mode::Standalone) {
      HeaderRemoveOperation* op;
      if (mode == Mode::ProxyToRequest)
        op = (HeaderRemoveOperation*)xqd_req_header_remove;
      else
        op = (HeaderRemoveOperation*)xqd_resp_header_remove;
      if (!HANDLE_RESULT(cx, op(detail::handle(self), name_chars.get(), name_len)))
        return false;
    }

    args.rval().setUndefined();
    return true;
  }

  bool forEach(JSContext* cx, unsigned argc, Value* vp) {
    HEADERS_ITERATION_METHOD(1)

    if (!args[0].isObject() || !JS::IsCallable(&args[0].toObject())) {
      JS_ReportErrorASCII(cx, "Failed to execute 'forEach' on 'Headers': "
                              "parameter 1 is not of type 'Function'");
      return false;
    }

    JS::RootedValueArray<3> newArgs(cx);
    newArgs[2].setObject(*self);

    RootedValue rval(cx);

    RootedValue iterable(cx);
    if (!JS::MapEntries(cx, backing_map, &iterable))
      return false;

    JS::ForOfIterator it(cx);
    if (!it.init(iterable))
      return false;

    RootedValue entry_val(cx);
    RootedObject entry(cx);
    while (true) {
      bool done;
      if (!it.next(&entry_val, &done))
        return false;

      if (done)
        break;

      entry = &entry_val.toObject();
      JS_GetElement(cx, entry, 1, newArgs[0]);
      JS_GetElement(cx, entry, 0, newArgs[1]);

      if (!JS::Call(cx, args.thisv(), args[0], newArgs, &rval))
        return false;
    }

    args.rval().setUndefined();
    return true;
  }

  bool entries(JSContext* cx, unsigned argc, Value* vp) {
    HEADERS_ITERATION_METHOD(0)
    return JS::MapEntries(cx, backing_map, args.rval());
  }

  bool keys(JSContext* cx, unsigned argc, Value* vp) {
    HEADERS_ITERATION_METHOD(0)
    return JS::MapKeys(cx, backing_map, args.rval());
  }

  bool values(JSContext* cx, unsigned argc, Value* vp) {
    HEADERS_ITERATION_METHOD(0)
    return JS::MapValues(cx, backing_map, args.rval());
  }

  const JSFunctionSpec methods[] = {
    JS_FN("get", get, 1, 0),
    JS_FN("has", has, 1, 0),
    JS_FN("set", set, 2, 0),
    JS_FN("append", append, 2, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("forEach", forEach, 1, 0),
    JS_FN("entries", entries, 0, 0),
    JS_FN("keys", keys, 0, 0),
    JS_FN("values", values, 0, 0),
    // [Symbol.iterator] added in init_class.
  JS_FS_END};

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE_CUSTOM_INIT(Headers)

  bool init_class(JSContext* cx, HandleObject global) {
    bool ok = init_class_impl(cx, global);
    if (!ok) return false;

    RootedValue entries(cx);
    if (!JS_GetProperty(cx, proto_obj, "entries", &entries))
      return false;

    JS::SymbolCode code = JS::SymbolCode::iterator;
    JS::RootedId iteratorId(cx, SYMBOL_TO_JSID(JS::GetWellKnownSymbol(cx, code)));
    return JS_DefinePropertyById(cx, proto_obj, iteratorId, entries, 0);
  }

  JSObject* create(JSContext* cx, Mode mode, HandleObject owner) {
    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    JS_SetReservedSlot(self, Slots::Mode, JS::Int32Value(static_cast<int32_t>(mode)));
    uint32_t handle = UINT32_MAX - 1;
    if (mode != Mode::Standalone)
      handle = RequestOrResponse::handle(owner);
    JS_SetReservedSlot(self, Slots::Handle, JS::Int32Value(static_cast<int32_t>(handle)));

    RootedObject backing_map(cx, JS::NewMapObject(cx));
    if (!backing_map) return nullptr;
    JS::SetReservedSlot(self, Slots::BackingMap, JS::ObjectValue(*backing_map));

    bool lazy = false;
    if ((mode == Mode::ProxyToRequest && Request::is_downstream(owner)) ||
        (mode == Mode::ProxyToResponse && Response::is_upstream(owner)))
    {
      lazy = true;
      if (!detail::get_header_names_from_handle(cx, handle, mode, backing_map))
        return nullptr;
    }

    JS_SetReservedSlot(self, Slots::HasLazyValues, JS::BooleanValue(lazy));

    return self;
  }
}

static JSString* get_geo_info(JSContext* cx, HandleString address_str) {
  size_t address_len;
  UniqueChars address = encode(cx, address_str, &address_len);
  if (!address) return nullptr;

  int format = AF_INET;
  size_t octets_len = 4;
  const char* caddress = address.get();
  for (size_t i = 0; i < address_len; i++) {
    if (caddress[i] == ':') {
      format = AF_INET6;
      octets_len = 16;
      break;
    }
  }

  char octets[sizeof(struct in6_addr)];
  if (inet_pton(format, caddress, octets) != 1) {
    // While get_geo_info can be invoked through FetchEvent#client.geo, too, that path can't
    // result in an invalid address here, so we can be more specific in the error message.
    JS_ReportErrorLatin1(cx, "Invalid address passed to fastly.getGeolocationForIpAddress");
    return nullptr;
  }

  OwnedHostCallBuffer buffer;
  size_t nwritten = 0;
  if (!HANDLE_RESULT(cx, xqd_geo_lookup(octets, octets_len, buffer.get(),
                     HOSTCALL_BUFFER_LEN, &nwritten)))
  {
    return nullptr;
  }

  return JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(buffer.get(), nwritten));
}

namespace ClientInfo {
  namespace Slots { enum {
    Address,
    GeoInfo,
    Count
  };};

  JSString* address(JSObject* obj) {
    JS::Value val = JS::GetReservedSlot(obj, Slots::Address);
    return val.isString() ? val.toString() : nullptr;
  }

  JSString* geo_info(JSObject* obj) {
    JS::Value val = JS::GetReservedSlot(obj, Slots::GeoInfo);
    return val.isString() ? val.toString() : nullptr;
  }

  static JSString* retrieve_address(JSContext* cx, HandleObject self) {
    RootedString address(cx);
    char octets[16];

    size_t nwritten = 0;
    if (!HANDLE_RESULT(cx, xqd_req_downstream_client_ip_addr_get(octets, &nwritten)))
      return nullptr;

    switch (nwritten) {
      case 0: {
        // No address to be had, leave `address` as a nullptr.
        break;
      }
      case 4: {
        char address_chars[INET_ADDRSTRLEN];
        // TODO: do we need to do error handling here, or can we depend on the
        // host giving us a valid address?
        inet_ntop(AF_INET, octets, address_chars, INET_ADDRSTRLEN);
        address = JS_NewStringCopyZ(cx, address_chars);
        if (!address) return nullptr;

        break;
      }
      case 16: {
        char address_chars[INET6_ADDRSTRLEN];
        // TODO: do we need to do error handling here, or can we depend on the
        // host giving us a valid address?
        inet_ntop(AF_INET6, octets, address_chars, INET6_ADDRSTRLEN);
        address = JS_NewStringCopyZ(cx, address_chars);
        if (!address) return nullptr;

        break;
      }
    }

    JS::SetReservedSlot(self, Slots::Address, JS::StringValue(address));
    return address;
  }

  static JSString* retrieve_geo_info(JSContext* cx, HandleObject self) {
    RootedString address_str(cx, address(self));
    if (!address_str) {
      address_str = retrieve_address(cx, self);
      if (!address_str) return nullptr;
    }

    RootedString geo(cx, get_geo_info(cx, address_str));
    if (!geo) return nullptr;

    JS::SetReservedSlot(self, Slots::GeoInfo, JS::StringValue(geo));
    return geo;
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool address_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedString address_str(cx, address(self));
    if (!address_str) {
      address_str = retrieve_address(cx, self);
      if (!address_str) return false;
    }

    args.rval().setString(address_str);
    return true;
  }

  bool geo_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedString geo_info_str(cx, geo_info(self));
    if (!geo_info_str) {
      geo_info_str = retrieve_geo_info(cx, self);
      if (!geo_info_str) return false;
    }

    return JS_ParseJSON(cx, geo_info_str, args.rval());
  }

  const JSFunctionSpec methods[] = {
  JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("address", address_get, 0),
    JS_PSG("geo", geo_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE_NO_CTOR(ClientInfo)

  JSObject* create(JSContext* cx) {
    return JS_NewObjectWithGivenProto(cx, &class_, proto_obj);
  }
}


namespace FetchEvent {
  namespace Slots { enum {
    Dispatch,
    Request,
    State,
    PendingPromiseCount,
    DecPendingPromiseCountFunc,
    ClientInfo,
    Count
  };};

  bool is_instance(JSObject* obj);

static PersistentRooted<JSObject*> INSTANCE;

  namespace detail {
    void inc_pending_promise_count(JSObject* self) {
      MOZ_ASSERT(is_instance(self));
      auto count = JS::GetReservedSlot(self, Slots::PendingPromiseCount).toInt32();
      count++;
      MOZ_ASSERT(count > 0);
      JS::SetReservedSlot(self, Slots::PendingPromiseCount, JS::Int32Value(count));
    }

    void dec_pending_promise_count(JSObject* self) {
      MOZ_ASSERT(is_instance(self));
      auto count = JS::GetReservedSlot(self, Slots::PendingPromiseCount).toInt32();
      MOZ_ASSERT(count > 0);
      count--;
      JS::SetReservedSlot(self, Slots::PendingPromiseCount, JS::Int32Value(count));
    }

    bool add_pending_promise(JSContext* cx, HandleObject self, HandleObject promise) {
      MOZ_ASSERT(is_instance(self));
      MOZ_ASSERT(JS::IsPromiseObject(promise));
      RootedObject handler(cx);
      handler = &JS::GetReservedSlot(self, Slots::DecPendingPromiseCountFunc).toObject();
      if (!JS::AddPromiseReactions(cx, promise, handler, handler))
        return false;

      inc_pending_promise_count(self);
      return true;
    }
  }

  const unsigned ctor_length = 0;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool client_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    RootedValue clientInfo(cx, JS::GetReservedSlot(self, Slots::ClientInfo));

    if (clientInfo.isUndefined()) {
      RootedObject obj(cx, ClientInfo::create(cx));
      if (!obj)
        return false;
      clientInfo.setObject(*obj);
      JS::SetReservedSlot(self, Slots::ClientInfo, clientInfo);
    }

    args.rval().set(clientInfo);
    return true;
  }

  static JSObject* prepare_downstream_request(JSContext* cx) {
      return Request::create(cx, RequestHandle { INVALID_HANDLE },
                             BodyHandle { INVALID_HANDLE }, true);
  }

  static bool init_downstream_request(JSContext* cx, HandleObject request) {
    MOZ_RELEASE_ASSERT(Request::request_handle(request).handle == INVALID_HANDLE);

    RequestHandle request_handle = { INVALID_HANDLE };
    BodyHandle body_handle = { INVALID_HANDLE };
    if (!HANDLE_RESULT(cx, xqd_req_body_downstream_get(&request_handle, &body_handle)))
      return false;

    JS::SetReservedSlot(request, Request::Slots::Request, JS::Int32Value(request_handle.handle));
    JS::SetReservedSlot(request, RequestOrResponse::Slots::Body,
                        JS::Int32Value(body_handle.handle));

    size_t bytes_read;
    UniqueChars buf(read_from_handle_all<xqd_req_uri_get, RequestHandle>(cx, request_handle,
                                                                         &bytes_read, false));
    if (!buf) return false;

    RootedString url(cx, JS_NewStringCopyN(cx, buf.get(), bytes_read));
    if (!url) return false;
    JS::SetReservedSlot(request, Request::Slots::URL, JS::StringValue(url));

    // Set `fastly.baseURL` to the origin of the client request's URL.
    // Note that this only happens if baseURL hasn't already been set to another value explicitly.
    if (!Fastly::baseURL.get()) {
      // Extracting the origin part from the URL is a bit cumbersome, unfortunately.
      // We could create a JSUrl for the full URL and then reset its href to its origin, but that'd
      // incur a bunch of allocation and be more code than manually finding the third '/' and only
      // using the part before it. Since we're guaranteed to operate on an absolute URL, we're also
      // guaranteed to have an origin of the form `protocol://host:port`, with the "//" being the
      // only valid occurrence of slashes in the part we want.
      char* chars = buf.get();
      size_t origin_length = 0;
      size_t slashes = 0;
      for (size_t i = 0; i < bytes_read; i++) {
        if (chars[i] == '/' && ++slashes == 3) {
          origin_length = i;
          break;
        }
      }

      if (origin_length == 0) {
        origin_length = bytes_read;
      }

      SpecString spec((uint8_t*)buf.release(), origin_length, bytes_read);
      Fastly::baseURL = URL::create(cx, spec);
      if (!Fastly::baseURL) return false;
    }

    return true;
  }

  bool request_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    args.rval().set(JS::GetReservedSlot(self, Slots::Request));
    return true;
  }

  bool start_response(JSContext* cx, HandleObject response_obj, bool streaming) {
    ResponseHandle response = Response::response_handle(response_obj);
    BodyHandle body = RequestOrResponse::body_handle(response_obj);

    return HANDLE_RESULT(cx, xqd_resp_send_downstream(response, body, streaming));
  }

  bool respond_blocking(JSContext* cx, HandleObject response_obj) {
    return start_response(cx, response_obj, false);
  }

  bool body_reader_then_handler(JSContext* cx, HandleObject response, CallArgs args) {
    RootedObject then_handler(cx, &args.callee());
    // The reader is stored in the catch handler, which we need here as well.
    // So we get that first, then the reader.
    RootedObject catch_handler(cx, &js::GetFunctionNativeReserved(then_handler, 1).toObject());
    RootedObject reader(cx, &js::GetFunctionNativeReserved(catch_handler, 1).toObject());
    BodyHandle body_handle = RequestOrResponse::body_handle(response);

    // We're guaranteed to work with a native ReadableStreamDefaultReader here, which in turn is
    // guaranteed to vend {done: bool, value: any} objects to read promise then callbacks.
    RootedObject chunk_obj(cx, &args[0].toObject());
    RootedValue done_val(cx);
    if (!JS_GetProperty(cx, chunk_obj, "done", &done_val))
      return false;

    if (done_val.toBoolean()) {
      FetchEvent::set_state(FetchEvent::instance(), FetchEvent::State::responseDone);
      return HANDLE_RESULT(cx, xqd_body_close(body_handle));
    }

    RootedValue val(cx);
    if (!JS_GetProperty(cx, chunk_obj, "value", &val))
      return false;

    if (!val.isObject() || !JS_IsUint8Array(&val.toObject())) {
      // TODO: check if this should create a rejected promise instead, so an in-content handler
      // for unhandled rejections could deal with it.
      // The read operation returned a chunk that's not a Uint8Array.
      fprintf(stderr, "Error: read operation on body ReadableStream didn't respond with a "
                      "Uint8Array. Received value: ");
      dump_value(cx, val, stderr);
      return false;
    }

    {
      JS::AutoCheckCannotGC nogc;
      JSObject* array = &val.toObject();
      bool is_shared;
      uint8_t* bytes = JS_GetUint8ArrayData(array, &is_shared, nogc);
      size_t length = JS_GetTypedArrayByteLength(array);
      size_t nwritten;
      if (!HANDLE_RESULT(cx, xqd_body_write(body_handle, (char*)bytes, length,
                                            BodyWriteEndBack, &nwritten)))
      {
        return false;
      }
    }

    // Read the next chunk.
    RootedObject promise(cx, JS::ReadableStreamDefaultReaderRead(cx, reader));
    if (!promise) return false;
    return JS::AddPromiseReactions(cx, promise, then_handler, catch_handler);
  }

  bool body_reader_catch_handler(JSContext* cx, HandleObject response, CallArgs args) {
    RootedObject reader(cx, &js::GetFunctionNativeReserved(&args.callee(), 1).toObject());

    // TODO: check if this should create a rejected promise instead, so an in-content handler
    // for unhandled rejections could deal with it.
    // The body stream errored during the streaming response.
    // Not much we can do, but at least close the stream, and warn.
    fprintf(stderr, "Warning: body ReadableStream closed during streaming response. Exception: ");
    dump_value(cx, args.get(0), stderr);

    FetchEvent::set_state(FetchEvent::instance(), FetchEvent::State::responseDone);
    return HANDLE_RESULT(cx, xqd_body_close(RequestOrResponse::body_handle(response)));
  }

  bool respond_maybe_streaming(JSContext* cx, HandleObject response_obj, bool* streaming) {
    RootedObject stream(cx, RequestOrResponse::body_stream(response_obj));
    MOZ_ASSERT(stream);

    bool locked_or_disturbed;
    if (!JS::ReadableStreamIsLocked(cx, stream, &locked_or_disturbed))
      return false;
    if (!locked_or_disturbed && !JS::ReadableStreamIsDisturbed(cx, stream, &locked_or_disturbed))
      return false;
    if (locked_or_disturbed) {
      // TODO: Improve this message; `disturbed` is probably too spec-internal a term.
      JS_ReportErrorUTF8(cx, "respondWith called with a Response containing "
                             "a body stream that's locked or disturbed");
      return false;
    }

    // If the body stream is backed by a C@E body handle, we can directly pipe that handle
    // into the response body we're about to send.
    if (NativeStreamSource::stream_is_body(cx, stream)) {
      // First, move the source's body handle to the target.
      RootedObject stream_source(cx, NativeStreamSource::get_stream_source(cx, stream));
      RootedObject source_owner(cx, NativeStreamSource::owner(stream_source));
      if (!RequestOrResponse::move_body_handle(cx, source_owner, response_obj))
        return false;

      // Then, send the response without streaming. We know that content won't append to this body
      // handle, because we don't expose any means to do so, so it's ok for it to be closed
      // immediately.
      *streaming = false;
      return start_response(cx, response_obj, false);
    }

    RootedObject reader(cx, JS::ReadableStreamGetReader(cx, stream,
                                                        JS::ReadableStreamReaderMode::Default));
    if (!reader) return false;

    bool is_closed;
    if (!JS::ReadableStreamReaderIsClosed(cx, reader, &is_closed))
      return false;

    // It's ok for the stream to be closed, as its contents might
    // already have fully been written to the body handle.
    // In that case, we can do a blocking send instead.
    if (is_closed) {
      *streaming = false;
      return start_response(cx, response_obj, false);
    }

    // Create handlers for both `then` and `catch`.
    // These are functions with two reserved slots, in which we store all information required
    // to perform the reactions.
    // We store the actually required information on the catch handler, and a reference to that
    // on the then handler. This allows us to reuse these functions for the next read operation
    // in the then handler. The catch handler won't ever have a need to perform another operation
    // in this way.
    RootedObject catch_handler(cx);
    catch_handler = create_internal_method<body_reader_catch_handler>(cx, response_obj);
    if (!catch_handler) return false;

    js::SetFunctionNativeReserved(catch_handler, 1, JS::ObjectValue(*reader));

    RootedObject then_handler(cx);
    then_handler = create_internal_method<body_reader_then_handler>(cx, response_obj);
    if (!then_handler) return false;

    js::SetFunctionNativeReserved(then_handler, 1, JS::ObjectValue(*catch_handler));

    RootedObject promise(cx, JS::ReadableStreamDefaultReaderRead(cx, reader));
    if (!promise) return false;
    if (!JS::AddPromiseReactions(cx, promise, then_handler, catch_handler))
      return false;

    if (!start_response(cx, response_obj, true))
      return false;

    *streaming = true;
    return true;
  }

  // Steps in this function refer to the spec at
  // https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
  bool response_promise_then_handler(JSContext* cx, HandleObject event, CallArgs args) {
    // Step 10.1
    // Note: the `then` handler is only invoked after all Promise resolution has happened.
    // (Even if there were multiple Promises to unwrap first.)
    // That means that at this point we're guaranteed to have the final value instead of a
    // Promise wrapping it, so either the value is a Response, or we have to bail.
    if (!Response::is_instance(args.get(0))) {
      JS_ReportErrorUTF8(cx, "FetchEvent#respondWith must be called with a Response "
                             "object or a Promise resolving to a Response object as "
                             "the first argument");
      RootedObject rejection(cx, PromiseRejectedWithPendingError(cx));
      if (!rejection) return false;
      args.rval().setObject(*rejection);
      return respondWithError(cx, event);
    }

    // Step 10.2 (very roughly: the way we handle responses and their bodies is very different.)
    RootedObject response_obj(cx, &args[0].toObject());

    // Ensure that all headers are stored client-side, so we retain access to them after
    // sending the response off.
    if (Response::is_upstream(response_obj)) {
        RootedObject headers(cx);
        headers = RequestOrResponse::headers<Headers::Mode::ProxyToResponse>(cx, response_obj);
        if (!Headers::delazify(cx, headers))
          return false;
    }

    bool streaming = false;
    if (RequestOrResponse::body_stream(response_obj)) {
      if (!respond_maybe_streaming(cx, response_obj, &streaming))
        return false;
    } else {
      if (!respond_blocking(cx, response_obj))
        return false;
    }

    set_state(event, streaming ? State::responseStreaming : State::responseDone);
    return true;
  }

  // Steps in this function refer to the spec at
  // https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
  bool response_promise_catch_handler(JSContext* cx, HandleObject event, CallArgs args) {
    RootedObject promise(cx, &js::GetFunctionNativeReserved(&args.callee(), 1).toObject());

    fprintf(stderr, "Error while running request handler: ");
    dump_promise_rejection(cx, args.get(0), promise, stderr);

    // TODO: verify that this is the right behavior.
    // Steps 9.1-2
    return respondWithError(cx, event);
  }

  // Steps in this function refer to the spec at
  // https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
  bool respondWith(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    // Coercion of argument `r` to a Promise<Response>
    RootedObject response_promise(cx, JS::CallOriginalPromiseResolve(cx, args.get(0)));
    if (!response_promise) return false;

    // Step 2
    if (!is_dispatching(self)) {
      JS_ReportErrorUTF8(cx, "FetchEvent#respondWith must be called synchronously from "
                             "within a FetchEvent handler");
      return false;
    }

    // Step 3
    if (state(self) != State::unhandled) {
      JS_ReportErrorUTF8(cx,
                         "FetchEvent#respondWith can't be called twice on the same event");
      return false;
    }

    // Step 4
    detail::add_pending_promise(cx, self, response_promise);

    // Steps 5-7 (very roughly)
    set_state(self, State::waitToRespond);

    // Step 9 (continued in `response_promise_catch_handler` above)
    RootedObject catch_handler(cx);
    catch_handler = create_internal_method<response_promise_catch_handler>(cx, self);
    if (!catch_handler) return false;

    js::SetFunctionNativeReserved(catch_handler, 1, JS::ObjectValue(*response_promise));

    // Step 10 (continued in `response_promise_then_handler` above)
    RootedObject then_handler(cx);
    then_handler = create_internal_method<response_promise_then_handler>(cx, self);
    if (!then_handler) return false;

    if (!JS::AddPromiseReactions(cx, response_promise, then_handler, catch_handler))
      return false;

    args.rval().setUndefined();
    return true;
  }

  bool respondWithError(JSContext* cx, HandleObject self) {
    MOZ_RELEASE_ASSERT(state(self) == State::unhandled || state(self) == State::waitToRespond);
    set_state(self, State::responsedWithError);
    ResponseHandle response { INVALID_HANDLE };
    BodyHandle body { INVALID_HANDLE };
    return HANDLE_RESULT(cx, xqd_resp_new(&response)) &&
          HANDLE_RESULT(cx, xqd_body_new(&body)) &&
          HANDLE_RESULT(cx, xqd_resp_status_set(response, 500)) &&
          HANDLE_RESULT(cx, xqd_resp_send_downstream(response, body, false));
  }

  // Step 5 of https://w3c.github.io/ServiceWorker/#wait-until-method
  bool dec_pending_promise_count(JSContext* cx, HandleObject event, CallArgs args) {
    // Step 5.1
    detail::dec_pending_promise_count(event);

    // Note: step 5.2 not relevant to our implementation.
    return true;
  }

  // Steps in this function refer to the spec at
  // https://w3c.github.io/ServiceWorker/#wait-until-method
  bool waitUntil(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)

    RootedObject promise(cx, JS::CallOriginalPromiseResolve(cx, args.get(0)));
    if (!promise) return false;

    // Step 2
    if (!is_active(self)) {
      JS_ReportErrorUTF8(cx, "FetchEvent#waitUntil called on inactive event");
      return false;
    }

    // Steps 3-4
    detail::add_pending_promise(cx, self, promise);

    // Note: step 5 implemented in dec_pending_promise_count

    args.rval().setUndefined();
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("respondWith", respondWith, 1, 0),
    JS_FN("waitUntil", waitUntil, 1, 0),
  JS_FS_END};

  const JSPropertySpec properties[] = {
    JS_PSG("client", client_get, 0),
    JS_PSG("request", request_get, 0),
  JS_PS_END};

  CLASS_BOILERPLATE_NO_CTOR(FetchEvent)

  JSObject* create(JSContext* cx) {
    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    RootedObject request(cx, prepare_downstream_request(cx));
    if (!request) return nullptr;

    RootedObject dec_count_handler(cx, create_internal_method<dec_pending_promise_count>(cx, self));
    if (!dec_count_handler) return nullptr;

    JS::SetReservedSlot(self, Slots::Request, JS::ObjectValue(*request));
    JS::SetReservedSlot(self, Slots::Dispatch, JS::FalseValue());
    JS::SetReservedSlot(self, Slots::State, JS::Int32Value((int)State::unhandled));
    JS::SetReservedSlot(self, Slots::PendingPromiseCount, JS::Int32Value(0));
    JS::SetReservedSlot(self, Slots::DecPendingPromiseCountFunc,
                        JS::ObjectValue(*dec_count_handler));

    INSTANCE.init(cx, self);
    return self;
  }

  HandleObject instance() {
    return INSTANCE;
  }

  bool init_request(JSContext* cx, HandleObject self) {
    RootedObject request(cx, &JS::GetReservedSlot(self, Slots::Request).toObject());
    return init_downstream_request(cx, request);
  }

  bool is_active(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    // Note: we also treat the FetchEvent as active if it's in `responseStreaming` state
    // because that requires us to extend the service's lifetime as well.
    // In the spec this is achieved using individual promise counts for the body read operations.
    return JS::GetReservedSlot(self, Slots::Dispatch).toBoolean() ||
           state(self) == State::responseStreaming ||
           JS::GetReservedSlot(self, Slots::PendingPromiseCount).toInt32() > 0;
  }

  bool is_dispatching(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return JS::GetReservedSlot(self, Slots::Dispatch).toBoolean();
  }

  void start_dispatching(JSObject* self) {
    MOZ_ASSERT(!is_dispatching(self));
    JS::SetReservedSlot(self, Slots::Dispatch, JS::TrueValue());
  }

  void stop_dispatching(JSObject* self) {
    MOZ_ASSERT(is_dispatching(self));
    JS::SetReservedSlot(self, Slots::Dispatch, JS::FalseValue());
  }

  State state(JSObject* self) {
    MOZ_ASSERT(is_instance(self));
    return (State)JS::GetReservedSlot(self, Slots::State).toInt32();
  }

  void set_state(JSObject* self, State new_state) {
    MOZ_ASSERT(is_instance(self));
    MOZ_ASSERT((uint8_t)new_state > (uint8_t)state(self));
    JS::SetReservedSlot(self, Slots::State, JS::Int32Value((int)new_state));
  }

  bool response_started(JSObject* self) {
    State current_state = state(self);
    return current_state != State::unhandled && current_state != State::waitToRespond;
  }
}


namespace URLSearchParams {
  JSObject* create(JSContext* cx, jsurl::JSUrl* url);
  JSUrlSearchParams* get_params(JSObject* self);
}

namespace URL {
  namespace Slots { enum {
    Url,
    Params,
    Count
  };};

  const unsigned ctor_length = 1;

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);
  JSObject* create(JSContext* cx, HandleValue url_val, HandleValue base_val);

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "URL", 1))
      return false;

    RootedObject self(cx, create(cx, args.get(0), args.get(1)));
    if (!self) return false;

    args.rval().setObject(*self);
    return true;
  }

#define ACCESSOR_GET(field) \
  bool field##_get(JSContext* cx, unsigned argc, Value* vp) { \
    METHOD_HEADER(0) \
    const JSUrl* url = (JSUrl*)JS::GetReservedSlot(self, Slots::Url).toPrivate(); \
    const SpecSlice slice = jsurl::field(url); \
    RootedString str(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)slice.data, slice.len))); \
    if (!str) return false; \
    args.rval().setString(str); \
    return true; \
  } \

#define ACCESSOR_SET(field) \
  bool field##_set(JSContext* cx, unsigned argc, Value* vp) { \
    METHOD_HEADER(1) \
    JSUrl* url = (JSUrl*)JS::GetReservedSlot(self, Slots::Url).toPrivate(); \
    \
    SpecString str = encode(cx, args.get(0)); \
    if (!str.data) return false; \
    jsurl::set_##field(url, &str); \
    \
    args.rval().set(args.get(0)); \
    return true; \
  }

#define ACCESSOR(field) \
  ACCESSOR_GET(field) \
  ACCESSOR_SET(field)

  ACCESSOR(hash)
  ACCESSOR(host)
  ACCESSOR(hostname)
  ACCESSOR(href)
  ACCESSOR(password)
  ACCESSOR(pathname)
  ACCESSOR(port)
  ACCESSOR(protocol)
  ACCESSOR(search)
  ACCESSOR(username)

  bool origin_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    const JSUrl* url = (JSUrl*)JS::GetReservedSlot(self, Slots::Url).toPrivate();
    SpecString slice = jsurl::origin(url);
    RootedString str(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)slice.data, slice.len)));
    if (!str) return false;
    args.rval().setString(str);
    return true;
  }

  bool searchParams_get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    JS::Value params_val = JS::GetReservedSlot(self, Slots::Params);
    RootedObject params(cx);
    if (params_val.isNullOrUndefined()) {
      JSUrl* url = (JSUrl*)JS::GetReservedSlot(self, Slots::Url).toPrivate();
      params = URLSearchParams::create(cx, url);
      if (!params) return false;
      JS::SetReservedSlot(self, Slots::Params, JS::ObjectValue(*params));
    } else {
      params = &params_val.toObject();
    }

    args.rval().setObject(*params);
    return true;
  }

  bool toString(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return href_get(cx, argc, vp);
  }

  bool toJSON(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    return href_get(cx, argc, vp);
  }

  const JSFunctionSpec methods[] = {
    JS_FN("toString", toString, 0, JSPROP_ENUMERATE),
    JS_FN("toJSON", toJSON, 0, JSPROP_ENUMERATE),
    JS_FS_END
  };

  const JSPropertySpec properties[] = {
    JS_PSGS("hash", hash_get, hash_set, JSPROP_ENUMERATE),
    JS_PSGS("host", host_get, host_set, JSPROP_ENUMERATE),
    JS_PSGS("hostname", hostname_get, hostname_set, JSPROP_ENUMERATE),
    JS_PSGS("href", href_get, href_set, JSPROP_ENUMERATE),
    JS_PSG("origin", origin_get, JSPROP_ENUMERATE),
    JS_PSGS("password", password_get, password_set, JSPROP_ENUMERATE),
    JS_PSGS("pathname", pathname_get, pathname_set, JSPROP_ENUMERATE),
    JS_PSGS("port", port_get, port_set, JSPROP_ENUMERATE),
    JS_PSGS("protocol", protocol_get, protocol_set, JSPROP_ENUMERATE),
    JS_PSGS("search", search_get, search_set, JSPROP_ENUMERATE),
    JS_PSG("searchParams", searchParams_get, JSPROP_ENUMERATE),
    JS_PSGS("username", username_get, username_set, JSPROP_ENUMERATE),
    JS_PS_END
  };

  CLASS_BOILERPLATE(URL)

  JSObject* create(JSContext* cx, SpecString url_str, const JSUrl* base) {
    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    JSUrl* url;
    if (base) {
      url = jsurl::new_jsurl_with_base(&url_str, base);
    } else {
      url = jsurl::new_jsurl(&url_str);
    }

    if (!url) {
      JS_ReportErrorUTF8(cx, "URL constructor: %s is not a valid URL.", (char*)url_str.data);
      return nullptr;
    }

    JS::SetReservedSlot(self, Slots::Url, JS::PrivateValue(url));

    return self;
  }

  JSObject* create(JSContext* cx, HandleValue url_val, const JSUrl* base) {
    auto str = encode(cx, url_val);
    if (!str.data) return nullptr;

    return create(cx, str, base);
  }

  JSObject* create(JSContext* cx, HandleValue url_val, HandleObject base_obj) {
    MOZ_RELEASE_ASSERT(is_instance(base_obj));
    const JSUrl* base = (JSUrl*)JS::GetReservedSlot(base_obj, Slots::Url).toPrivate();

    return create(cx, url_val, base);
  }

  JSObject* create(JSContext* cx, HandleValue url_val, HandleValue base_val) {
    if (is_instance(base_val)) {
      RootedObject base_obj(cx, &base_val.toObject());
      return create(cx, url_val, base_obj);
    }

    JSUrl* base = nullptr;

    if (!base_val.isUndefined()) {
      auto str = encode(cx, base_val);
      if (!str.data) return nullptr;

      base = jsurl::new_jsurl(&str);
      if (!base) {
        JS_ReportErrorUTF8(cx, "URL constructor: %s is not a valid URL.", (char*)str.data);
        return nullptr;
      }
    }

    return create(cx, url_val, base);
  }
}


#define ITERTYPE_ENTRIES 0
#define ITERTYPE_KEYS 1
#define ITERTYPE_VALUES 2

namespace URLSearchParamsIterator {
  namespace Slots { enum {
    Params,
    Type,
    Index,
    Count
  };};

  const unsigned ctor_length = 0;
  // This constructor will be deleted from the class prototype right after class initialization.
  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    MOZ_RELEASE_ASSERT(false, "Should be deleted");
    return false;
  }

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);

  bool next(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    RootedObject params_obj(cx, &JS::GetReservedSlot(self, Slots::Params).toObject());
    const auto params = URLSearchParams::get_params(params_obj);
    size_t index = JS::GetReservedSlot(self, Slots::Index).toInt32();
    uint8_t type = JS::GetReservedSlot(self, Slots::Type).toInt32();

    RootedObject result(cx, JS_NewPlainObject(cx));
    if (!result) return false;

    auto param = JSSearchParam(SpecSlice(nullptr, 0), SpecSlice(nullptr, 0), false);
    jsurl::params_at(params, index, &param);

    if (param.done) {
      JS_DefineProperty(cx, result, "done", true, JSPROP_ENUMERATE);
      JS_DefineProperty(cx, result, "value", JS::UndefinedHandleValue, JSPROP_ENUMERATE);

      args.rval().setObject(*result);
      return true;
    }

    JS_DefineProperty(cx, result, "done", false, JSPROP_ENUMERATE);

    RootedValue key_val(cx);
    RootedValue val_val(cx);

    if (type != ITERTYPE_VALUES) {
      auto chars = JS::UTF8Chars((char*)param.name.data, param.name.len);
      RootedString str(cx, JS_NewStringCopyUTF8N(cx, chars));
      if (!str) return false;
      key_val = JS::StringValue(str);
    }

    if (type != ITERTYPE_KEYS) {
      auto chars = JS::UTF8Chars((char*)param.value.data, param.value.len);
      RootedString str(cx, JS_NewStringCopyUTF8N(cx, chars));
      if (!str) return false;
      val_val = JS::StringValue(str);
    }

    RootedValue result_val(cx);

    switch (type) {
      case ITERTYPE_ENTRIES: {
        RootedObject pair(cx, JS::NewArrayObject(cx, 2));
        if (!pair) return false;
        JS_DefineElement(cx, pair, 0, key_val, JSPROP_ENUMERATE);
        JS_DefineElement(cx, pair, 1, val_val, JSPROP_ENUMERATE);
        result_val = JS::ObjectValue(*pair);
        break;
      }
      case ITERTYPE_KEYS: {
        result_val = key_val;
        break;
      }
      case ITERTYPE_VALUES: {
        result_val = val_val;
        break;
      }
      default:
        MOZ_RELEASE_ASSERT(false, "Invalid iter type");
    }

    JS_DefineProperty(cx, result, "value", result_val, JSPROP_ENUMERATE);

    JS::SetReservedSlot(self, Slots::Index, JS::Int32Value(index + 1));
    args.rval().setObject(*result);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("next", next, 0, JSPROP_ENUMERATE),
  JS_FS_END};

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE_CUSTOM_INIT(URLSearchParamsIterator)

  bool init_class(JSContext* cx, HandleObject global) {
    RootedObject iterator_proto(cx, JS::GetRealmIteratorPrototype(cx));
    if (!iterator_proto) return false;

    if (!init_class_impl(cx, global, iterator_proto))
      return false;

    // Delete both the `URLSearchParamsIterator` global property and the `constructor`
    // property on `URLSearchParamsIterator.prototype`.
    // The latter because Iterators don't have their own constructor on the prototype.
    return JS_DeleteProperty(cx, global, class_.name) &&
           JS_DeleteProperty(cx, proto_obj, "constructor");
  }

  JSObject* create(JSContext* cx, HandleObject params, uint8_t type) {
    MOZ_RELEASE_ASSERT(type <= ITERTYPE_VALUES);

    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    JS::SetReservedSlot(self, Slots::Params, JS::ObjectValue(*params));
    JS::SetReservedSlot(self, Slots::Type, JS::Int32Value(type));
    JS::SetReservedSlot(self, Slots::Index, JS::Int32Value(0));

    return self;
  }
}


namespace URLSearchParams {
  namespace Slots { enum {
    Url,
    Params,
    Count
  };};

  namespace detail {
    bool append(JSContext* cx, HandleObject self, HandleValue key, HandleValue val, const char* _) {
      const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

      auto name = encode(cx, key);
      if (!name.data) return false;

      auto value = encode(cx, val);
      if (!value.data) return false;

      jsurl::params_append(params, name, value);
      return true;
    }
  }

  JSUrlSearchParams* get_params(JSObject* self) {
    return (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();
  }

  bool check_receiver(JSContext* cx, HandleValue receiver, const char* method_name);
  JSObject* create(JSContext* cx, HandleValue params_val);

  const unsigned ctor_length = 1;

  bool constructor(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject self(cx, create(cx, args.get(0)));
    if (!self) return false;

    args.rval().setObject(*self);
    return true;
  }

  bool append(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(2)
    if (!detail::append(cx, self, args[0], args[1], "append"))
      return false;

    args.rval().setUndefined();
    return true;
  }

  bool delete_(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER_WITH_NAME(1, "delete")
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

    auto name = encode(cx, args.get(0));
    if (!name.data) return false;

    jsurl::params_delete(params, &name);
    args.rval().setUndefined();
    return true;
  }

  bool has(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

    auto name = encode(cx, args.get(0));
    if (!name.data) return false;

    args.rval().setBoolean(jsurl::params_has(params, &name));
    return true;
  }

  bool get(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

    auto name = encode(cx, args.get(0));
    if (!name.data) return false;

    const SpecSlice slice = jsurl::params_get(params, &name);
    if (!slice.data) {
      args.rval().setNull();
      return true;
    }

    RootedString str(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)slice.data, slice.len)));
    if (!str) return false;
    args.rval().setString(str);
    return true;
  }

  bool getAll(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

    auto name = encode(cx, args.get(0));
    if (!name.data) return false;

    const jsurl::CVec<SpecSlice> values = jsurl::params_get_all(params, &name);

    RootedObject result(cx, JS::NewArrayObject(cx, values.len));
    if (!result) return false;

    RootedString str(cx);
    RootedValue str_val(cx);
    for (size_t i = 0; i < values.len; i++) {
      const SpecSlice value = values.ptr[i];
      str = JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)value.data, value.len));
      if (!str) return false;

      str_val.setString(str);
      if (!JS_SetElement(cx, result, i, str_val))
        return false;
    }

    args.rval().setObject(*result);
    return true;
  }

  bool set(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(2)
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

    auto name = encode(cx, args[0]);
    if (!name.data) return false;

    auto value = encode(cx, args[1]);
    if (!value.data) return false;

    jsurl::params_set(params, name, value);
    return true;

    args.rval().setUndefined();
    return true;
  }

  bool sort(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();
    jsurl::params_sort(params);
    args.rval().setUndefined();
    return true;
  }

  bool toString(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)
    const auto params = (JSUrlSearchParams*)JS::GetReservedSlot(self, Slots::Params).toPrivate();

    const SpecSlice slice = jsurl::params_to_string(params);
    RootedString str(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)slice.data, slice.len)));
    if (!str) return false;

    args.rval().setString(str);
    return true;
  }

  bool forEach(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(1)
    const auto params = get_params(self);

    if (!args[0].isObject() || !JS::IsCallable(&args[0].toObject())) {
      JS_ReportErrorASCII(cx, "Failed to execute 'forEach' on 'URLSearchParams': "
                              "parameter 1 is not of type 'Function'");
      return false;
    }

    HandleValue callback = args[0];
    HandleValue thisv = args.get(1);

    JS::RootedValueArray<3> newArgs(cx);
    newArgs[2].setObject(*self);
    RootedValue rval(cx);

    auto param = JSSearchParam(SpecSlice(nullptr, 0), SpecSlice(nullptr, 0), false);
    RootedString name_str(cx);
    RootedString val_str(cx);
    size_t index = 0;
    while (true) {
      jsurl::params_at(params, index, &param);
      if (param.done)
        break;

      name_str = JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)param.name.data, param.name.len));
      if (!name_str) return false;
      newArgs[1].setString(name_str);

      val_str = JS_NewStringCopyUTF8N(cx, JS::UTF8Chars((char*)param.value.data, param.value.len));
      if (!val_str) return false;
      newArgs[0].setString(val_str);

      if (!JS::Call(cx, thisv, callback, newArgs, &rval))
        return false;

      index++;
    }

    args.rval().setUndefined();
    return true;
  }

  template<auto type>
  bool get_iter(JSContext* cx, unsigned argc, Value* vp) {
    METHOD_HEADER(0)

    RootedObject iter(cx, URLSearchParamsIterator::create(cx, self, type));
    if (!iter) return false;
    args.rval().setObject(*iter);
    return true;
  }

  const JSFunctionSpec methods[] = {
    JS_FN("append", append, 2, JSPROP_ENUMERATE),
    JS_FN("delete", delete_, 1, JSPROP_ENUMERATE),
    JS_FN("has", has, 1, JSPROP_ENUMERATE),
    JS_FN("get", get, 1, JSPROP_ENUMERATE),
    JS_FN("getAll", getAll, 1, JSPROP_ENUMERATE),
    JS_FN("set", set, 2, JSPROP_ENUMERATE),
    JS_FN("sort", sort, 0, JSPROP_ENUMERATE),
    JS_FN("toString", toString, 0, JSPROP_ENUMERATE),
    JS_FN("forEach", forEach, 0, JSPROP_ENUMERATE),
    JS_FN("entries", get_iter<ITERTYPE_ENTRIES>, 0, 0),
    JS_FN("keys", get_iter<ITERTYPE_KEYS>, 0, 0),
    JS_FN("values", get_iter<ITERTYPE_VALUES>, 0, 0),
    // [Symbol.iterator] added in init_class.
    JS_FS_END
  };

  const JSPropertySpec properties[] = {JS_PS_END};

  CLASS_BOILERPLATE_CUSTOM_INIT(URLSearchParams)

  bool init_class(JSContext* cx, HandleObject global) {
    if (!init_class_impl(cx, global))
      return false;

    RootedValue entries(cx);
    if (!JS_GetProperty(cx, proto_obj, "entries", &entries))
      return false;

    JS::SymbolCode code = JS::SymbolCode::iterator;
    JS::RootedId iteratorId(cx, SYMBOL_TO_JSID(JS::GetWellKnownSymbol(cx, code)));
    return JS_DefinePropertyById(cx, proto_obj, iteratorId, entries, 0);
  }

  JSObject* create(JSContext* cx, HandleValue params_val) {
    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    auto params = jsurl::new_params();
    JS::SetReservedSlot(self, Slots::Params, JS::PrivateValue(params));

    bool consumed = false;
    const char* alt_text = ", or a value that can be stringified";
    if (!maybe_consume_sequence_or_record<detail::append>(cx, params_val, self, &consumed,
                                                          "URLSearchParams", alt_text))
    {
      return nullptr;
    }

    if (!consumed) {
      auto init = encode(cx, params_val);
      if (!init.data) return nullptr;

      jsurl::params_init(params, &init);
    }

    return self;
  }

  JSObject* create(JSContext* cx, JSUrl* url) {
    RootedObject self(cx, JS_NewObjectWithGivenProto(cx, &class_, proto_obj));
    if (!self) return nullptr;

    JSUrlSearchParams* params = jsurl::url_search_params(url);
    if (!params) return nullptr;

    JS::SetReservedSlot(self, Slots::Params, JS::PrivateValue(params));
    JS::SetReservedSlot(self, Slots::Url, JS::PrivateValue(url));

    return self;
  }
}


// TODO: throw in all Request methods/getters that rely on host calls once a
// request has been sent. The host won't let us act on them anymore anyway.
bool fetch(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  REQUEST_HANDLER_ONLY("fetch")

  if (!args.requireAtLeast(cx, "fetch", 1)) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  RootedObject request(cx, Request::create(cx, args[0], args.get(1)));
  if (!request) {
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  RootedString backend(cx, Request::backend(request));
  if (!backend) {
    backend = Fastly::defaultBackend;
  }
  if (!backend) {
    size_t bytes_read;
    RequestHandle handle = Request::request_handle(request);
    UniqueChars buf(read_from_handle_all<xqd_req_uri_get, RequestHandle>(cx, handle, &bytes_read,
                                                                         false));
    if (buf) {
      JS_ReportErrorUTF8(cx, "No backend specified for request with url %s. "
                             "Must provide a `backend` property on the `init` object "
                             "passed to either `new Request()` or `fetch`", buf.get());
    }
    return ReturnPromiseRejectedWithPendingError(cx, args);
  }

  size_t backend_len;
  UniqueChars backend_chars = encode(cx, backend, &backend_len);
  if (!backend_chars)
    return ReturnPromiseRejectedWithPendingError(cx, args);

  PendingRequestHandle request_handle = { INVALID_HANDLE };
  RootedObject response_promise(cx, JS::NewPromiseObject(cx, nullptr));
  if (!response_promise)
    return ReturnPromiseRejectedWithPendingError(cx, args);

  // TODO: ensure that we properly handle body handles forwarded from other Request/Response
  // objects.
  int result = xqd_req_send_async(Request::request_handle(request),
                                  RequestOrResponse::body_handle(request),
                                  backend_chars.get(), backend_len, &request_handle);
  if (!HANDLE_RESULT(cx, result))
    return ReturnPromiseRejectedWithPendingError(cx, args);

  if (!pending_requests->append(request))
    return ReturnPromiseRejectedWithPendingError(cx, args);

  JS::SetReservedSlot(request, Request::Slots::PendingRequest, JS::Int32Value(request_handle.handle));
  JS::SetReservedSlot(request, Request::Slots::ResponsePromise, JS::ObjectValue(*response_promise));

  args.rval().setObject(*response_promise);
  return true;
}

bool has_pending_requests() {
  return pending_requests->length() > 0 || pending_body_reads->length() > 0;
}

bool process_pending_requests(JSContext* cx) {
  if (pending_requests->length() == 0) return true;

  size_t count = pending_requests->length();
  auto handles = mozilla::MakeUnique<PendingRequestHandle[]>(sizeof(PendingRequestHandle) * count);
  if (!handles)
    return false;

  for (size_t i = 0; i < count; i++) {
      handles[i] = Request::pending_handle((*pending_requests)[i]);
  }

  uint32_t done_index;
  ResponseHandle response_handle = { INVALID_HANDLE };
  BodyHandle body = { INVALID_HANDLE };
  int result = xqd_req_pending_req_select(handles.get(), count, &done_index,
                                          &response_handle, &body);
  if (!HANDLE_RESULT(cx, result))
    return false;

  HandleObject request = (*pending_requests)[done_index];
  RootedObject response_promise(cx, Request::response_promise(request));

  if (response_handle.handle == INVALID_HANDLE) {
    pending_requests->erase(const_cast<JSObject**>(request.address()));
    JS_ReportErrorUTF8(cx, "NetworkError when attempting to fetch resource.");
    RootedValue exn(cx);
    if (!JS_IsExceptionPending(cx) || !JS_GetPendingException(cx, &exn)) {
      return false;
    }
    JS_ClearPendingException(cx);
    return JS::RejectPromise(cx, response_promise, exn);
  }

  RootedObject response(cx, Response::create(cx, response_handle, body, true));
  if (!response) return false;

  RequestOrResponse::set_url(response, RequestOrResponse::url(request));
  RootedValue response_val(cx, JS::ObjectValue(*response));

  pending_requests->erase(const_cast<JSObject**>(request.address()));

  return JS::ResolvePromise(cx, response_promise, response_val);
}

bool error_stream_controller_with_pending_exception(JSContext* cx, HandleObject controller) {
  RootedValue exn(cx);
  if (!JS_GetPendingException(cx, &exn))
    return false;
  JS_ClearPendingException(cx);

  JS::RootedValueArray<1> args(cx);
  args[0].set(exn);
  RootedValue r(cx);
  return JS::Call(cx, controller, "error", args, &r);
}

bool process_next_body_read(JSContext* cx) {
  if (pending_body_reads->length() == 0) return true;

  RootedObject owner(cx);
  RootedObject controller(cx);
  {
    HandleObject streamSource = (*pending_body_reads)[0];
    owner = NativeStreamSource::owner(streamSource);
    controller = NativeStreamSource::controller(streamSource);
    pending_body_reads->erase(const_cast<JSObject**>(streamSource.address()));
  }

  BodyHandle body = RequestOrResponse::body_handle(owner);
  char* bytes = static_cast<char*>(JS_malloc(cx, HANDLE_READ_CHUNK_SIZE));
  if (!bytes) {
    return error_stream_controller_with_pending_exception(cx, controller);
  }
  size_t nwritten;
  int result = xqd_body_read(body, bytes, HANDLE_READ_CHUNK_SIZE, &nwritten);
  if (!HANDLE_RESULT(cx, result)) {
    JS_free(cx, bytes);
    return error_stream_controller_with_pending_exception(cx, controller);
  }

  if (nwritten == 0) {
    RootedValue r(cx);
    return JS::Call(cx, controller, "close", HandleValueArray::empty(), &r);
  }

  char* new_bytes = static_cast<char*>(JS_realloc(cx, bytes, HANDLE_READ_CHUNK_SIZE, nwritten));
  if (!new_bytes) {
    JS_free(cx, bytes);
    return error_stream_controller_with_pending_exception(cx, controller);
  }
  bytes = new_bytes;

  RootedObject buffer(cx, JS::NewArrayBufferWithContents(cx, nwritten, bytes));
  if (!buffer) {
    JS_free(cx, bytes);
    return error_stream_controller_with_pending_exception(cx, controller);
  }

  RootedObject byte_array(cx, JS_NewUint8ArrayWithBuffer(cx, buffer, 0, nwritten));
  if (!byte_array) return false;

  JS::RootedValueArray<1> enqueue_args(cx);
  enqueue_args[0].setObject(*byte_array);
  RootedValue r(cx);
  if (!JS::Call(cx, controller, "enqueue", enqueue_args, &r)) {
    return error_stream_controller_with_pending_exception(cx, controller);
  }

  return true;
}

bool process_network_io(JSContext* cx) {
  if (!has_pending_requests()) return true;

  if (!process_pending_requests(cx)) return false;

  if (!process_next_body_read(cx)) return false;

  return true;
}

bool define_fastly_sys(JSContext* cx, HandleObject global) {
  // Allocating the reusable hostcall buffer here means it's baked into the
  // snapshot, and since it's all zeros, it won't increase the size of the snapshot.
  if (!OwnedHostCallBuffer::initialize(cx)) return false;
  if (!JS_DefineProperty(cx, global, "self", global, JSPROP_ENUMERATE)) return false;

  if (!Fastly::create(cx, global)) return false;
  if (!Console::create(cx, global)) return false;
  if (!Crypto::create(cx, global)) return false;

  if (!NativeStreamSource::init_class(cx, global)) return false;
  if (!NativeStreamSink::init_class(cx, global)) return false;
  if (!TransformStreamDefaultController::init_class(cx, global)) return false;
  if (!TransformStream::init_class(cx, global)) return false;
  if (!Request::init_class(cx, global)) return false;
  if (!Response::init_class(cx, global)) return false;
  if (!Dictionary::init_class(cx, global)) return false;
  if (!Headers::init_class(cx, global)) return false;
  if (!ClientInfo::init_class(cx, global)) return false;
  if (!FetchEvent::init_class(cx, global)) return false;
  if (!CacheOverride::init_class(cx, global)) return false;
  if (!TextEncoder::init_class(cx, global)) return false;
  if (!TextDecoder::init_class(cx, global)) return false;
  if (!Logger::init_class(cx, global)) return false;
  if (!URL::init_class(cx, global)) return false;
  if (!URLSearchParams::init_class(cx, global)) return false;
  if (!URLSearchParamsIterator::init_class(cx, global)) return false;

  pending_requests = new JS::PersistentRootedObjectVector(cx);
  pending_body_reads = new JS::PersistentRootedObjectVector(cx);

  return JS_DefineFunction(cx, global, "fetch", fetch, 2, JSPROP_ENUMERATE);
}

JSObject* create_fetch_event(JSContext* cx) {
    return FetchEvent::create(cx);
}

UniqueChars stringify_value(JSContext* cx, JS::HandleValue value) {
  JS::RootedString str(cx, JS_ValueToSource(cx, value));
  if (!str)
  return nullptr;

  return JS_EncodeStringToUTF8(cx, str);
}

bool debug_logging_enabled() {
  return Fastly::debug_logging_enabled;
}

bool dump_value(JSContext* cx, JS::Value val, FILE* fp) {
  RootedValue value(cx, val);
  UniqueChars utf8chars = stringify_value(cx, value);
  if (!utf8chars)
    return false;
  fprintf(fp, "%s\n", utf8chars.get());
  return true;
}

bool print_stack(JSContext* cx, HandleObject stack, FILE* fp) {
  RootedString stackStr(cx);
  if (!BuildStackString(cx, nullptr, stack, &stackStr, 2)) {
    return false;
  }
  size_t stack_len;

  UniqueChars utf8chars = encode(cx, stackStr, &stack_len);
  if (!utf8chars)
    return false;
  fprintf(fp, "%s\n", utf8chars.get());
  return true;
}

void dump_promise_rejection(JSContext* cx, HandleValue reason, HandleObject promise, FILE* fp) {
  bool reported = false;
  RootedObject stack(cx);

  if (reason.isObject()) {
    RootedObject err(cx, &reason.toObject());
    JSErrorReport* report = JS_ErrorFromException(cx, err);
    if (report) {
      fprintf(stderr, "%s\n", report->message().c_str());
      reported = true;
    }

    stack = JS::ExceptionStackOrNull(err);
  }

  // If the rejection reason isn't an `Error` object, we just dump the value as-is.
  if (!reported) {
    dump_value(cx, reason, stderr);
  }

  // If the rejection reason isn't an `Error` object, we can't get an exception stack from it.
  // In that case, fall back to getting the stack from the promise resolution site.
  // These should be identical in many cases, such as for exceptions thrown in async functions,
  // but for some reason the resolution site stack seems to sometimes be wrong, so we only fall
  // back to it as a last resort.
  if (!stack) {
    stack = JS::GetPromiseResolutionSite(promise);
  }

  if (stack) {
    fprintf(stderr, "Stack:\n");
    print_stack(cx, stack, stderr);
  }
}

bool print_stack(JSContext* cx, FILE* fp) {
  RootedObject stackp(cx);
  if (!JS::CaptureCurrentStack(cx, &stackp))
    return false;
  return print_stack(cx, stackp, fp);
}
