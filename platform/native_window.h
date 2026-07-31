#pragma once

/**
 * @file native_window.h
 * @brief Native window handles, as a vocabulary type (namespace nx).
 *
 * Here rather than in the platform layer that produces them or the RHI that
 * consumes them, because both need the definition and neither may depend on
 * the other: the RHI sits above the platform layer, while IWindow has to be
 * able to return one.
 */

#include "core/foundation/core/foundation.h"

namespace nx {

/// Which native handles a window hands out, so the RHI knows which
/// surface-creation path to take.
enum class NativeWindowKind : u8 {
  None,
  Win32,   ///< window = HWND,            display = HINSTANCE
  Xlib,    ///< window = ::Window,        display = Display*
  Wayland, ///< window = wl_surface*,     display = wl_display*
  Cocoa,   ///< window = NSWindow*,       display = unused
  Android, ///< window = ANativeWindow*,  display = unused
  UIKit,   ///< window = CAMetalLayer*,   display = UIView*
};

/// Native handles for RHI surface creation. Never dereference these above the
/// RHI; they exist to be handed to vkCreate*SurfaceKHR and friends.
struct NativeWindowHandle {
  NativeWindowKind kind = NativeWindowKind::None;
  void *window = nullptr;
  void *display = nullptr;
};

} // namespace nx
