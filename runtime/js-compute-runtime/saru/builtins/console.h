#ifndef SARU_BUILTIN_CONSOLE_H
#define SARU_BUILTIN_CONSOLE_H

#include "saru/builtin.h"

namespace saru {

class Console : public BuiltinNoConstructor<Console> {
private:
public:
  static constexpr const char *class_name = "Console";
  enum LogType {
    Log,
    Info,
    Debug,
    Warn,
    Error,
  };
  enum Slots { Count };
  static const JSFunctionSpec methods[];
  static const JSPropertySpec properties[];

  static bool create(JSContext *cx, JS::HandleObject global);
};

} // namespace saru

#endif
