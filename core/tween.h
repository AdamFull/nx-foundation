#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/reflection/attributes.h"

#include <cmath>

namespace nx {

enum class Ease : u8 { Linear, Smooth, In, Out, InOut };

[[nodiscard]] inline f32 ease(const Ease kind, const f32 progress) noexcept {
  const f32 t = clamp(progress, 0.f, 1.f);
  switch (kind) {
  case Ease::Smooth:
    return t * t * (3.f - 2.f * t);
  case Ease::In:
    return t * t;
  case Ease::Out:
    return t * (2.f - t);
  case Ease::InOut:
    return t < 0.5f ? 2.f * t * t
                    : 1.f - std::pow(-2.f * t + 2.f, 2.f) * 0.5f;
  case Ease::Linear:
    return t;
  }
  return t;
}

/// Small serialisable timeline shared by ECS, UI, and game-side systems.
/// `advance` is deterministic and handles zero-duration tweens in one update.
struct Tween {
  f32 from = 0.f;
  f32 to = 0.f;
  f32 duration = 0.f;
  f32 elapsed = 0.f;
  Ease easing = Ease::Linear;
  bool playing = true;

  [[nodiscard]] f32 progress() const noexcept {
    return duration > 0.f ? clamp(elapsed / duration, 0.f, 1.f) : 1.f;
  }
  [[nodiscard]] f32 value() const noexcept {
    return from + (to - from) * ease(easing, progress());
  }
  [[nodiscard]] bool finished() const noexcept {
    return !playing;
  }
  f32 advance(const f32 dt) noexcept {
    if (playing) {
      elapsed = duration > 0.f ? min(duration, elapsed + max(0.f, dt))
                               : duration;
      if (elapsed >= duration)
        playing = false;
    }
    return value();
  }
};

} // namespace nx

NX_REFLECT_ENUM(nx::Ease, (Linear, "linear"), (Smooth, "smooth"),
                (In, "in"), (Out, "out"), (InOut, "in_out"))
