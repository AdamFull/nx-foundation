#pragma once

#include "core/foundation/core/foundation.h"

namespace nx {

enum class NativeWindowKind : u8 {
  None,
  Win32,
  Xlib,
  Wayland,
  Cocoa,
  Android,
  UIKit,
};

struct NativeWindowHandle {
  NativeWindowKind kind = NativeWindowKind::None;
  void *window = nullptr;
  void *display = nullptr;
};

}
