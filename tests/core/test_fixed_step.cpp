#include "framework/nxtest.h"

#include "core/foundation/core/fixed_step.h"

namespace {

constexpr f32 STEP = 1.f / 60.f;

}

TEST_CASE("fixed step: frames shorter than a step are held") {
  nx::fixed_step clock{{}};

  CHECK(clock.advance(1.f / 180.f) == 0u);
  CHECK(clock.advance(1.f / 180.f) == 0u);
  CHECK(clock.advance(1.f / 180.f) == 1u);
}

TEST_CASE("fixed step: alpha is where the frame sits between two steps") {
  nx::fixed_step clock{{}};

  CHECK(clock.alpha() == 0.f);
  CHECK(clock.advance(STEP * 0.5f) == 0u);
  CHECK(clock.alpha() == nxtest::Approx(0.5).epsilon(0.01));

  CHECK(clock.advance(STEP) == 1u);
  CHECK(clock.alpha() == nxtest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("fixed step: a long frame is capped and the backlog is dropped") {
  nx::fixed_step clock{{.max_steps = 4}};

  CHECK(clock.advance(1.f) == 4u);
  CHECK(clock.dropped() > 0u);

  CHECK(clock.advance(STEP) == 1u);
}

TEST_CASE("fixed step: a cap above the backlog drops nothing") {
  nx::fixed_step clock{{.max_steps = 1000}};

  CHECK(clock.advance(1.f) == 60u);
  CHECK(clock.dropped() == 0u);
}

TEST_CASE("fixed step: the backlog clamp bounds an absurd frame") {
  nx::fixed_step clock{{.max_steps = 1000}};

  CHECK(clock.advance(86400.f) == 60u);
}

TEST_CASE("fixed step: a negative or absent dt cannot poison the clock") {
  nx::fixed_step clock{{}};

  CHECK(clock.advance(-1.f) == 0u);
  CHECK(clock.advance(-0.5f) == 0u);
  CHECK(clock.alpha() == 0.f);

  CHECK(clock.advance(STEP) == 1u);
}

TEST_CASE("fixed step: peek answers what advance would take") {
  nx::fixed_step clock{{}};

  CHECK(clock.peek(STEP * 2.5f) == 2u);
  CHECK(clock.peek(STEP * 2.5f) == 2u);
  CHECK(clock.advance(STEP * 2.5f) == 2u);

  CHECK(clock.peek(0.f) == 0u);
  CHECK(clock.alpha() == nxtest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("fixed step: peek is capped where advance is") {
  nx::fixed_step clock{{.max_steps = 2}};

  CHECK(clock.peek(1.f) == 2u);
  CHECK(clock.advance(1.f) == 2u);
}

TEST_CASE("fixed step: a rate of zero never steps") {
  nx::fixed_step clock{{.hz = 0.f}};

  CHECK(clock.seconds() == 0.f);
  CHECK(clock.advance(1.f) == 0u);
  CHECK(clock.alpha() == 0.f);
  CHECK(clock.dropped() == 0u);
}

TEST_CASE("fixed step: a cap of zero still runs one step") {
  nx::fixed_step clock{{.max_steps = 0}};

  CHECK(clock.advance(1.f) == 1u);
}

TEST_CASE("fixed step: the rate is what was asked for") {
  nx::fixed_step slow{{.hz = 30.f}};
  CHECK(slow.seconds() == nxtest::Approx(1.f / 30.f));
  CHECK(slow.advance(1.f / 30.f) == 1u);

  nx::fixed_step fast{{.hz = 120.f}};
  CHECK(fast.seconds() == nxtest::Approx(1.f / 120.f));
  CHECK(fast.advance(1.f / 30.f) == 4u);
}

TEST_CASE("fixed step: a default-constructed clock runs at 60") {
  nx::fixed_step clock;

  CHECK(clock.seconds() == nxtest::Approx(STEP));
  CHECK(clock.advance(STEP) == 1u);
}

TEST_CASE("fixed step: with no cost recorded the cap is max_steps") {
  nx::fixed_step clock{{.max_steps = 4}};
  CHECK(clock.cap() == 4u);
  CHECK(clock.step_cost() == 0.f);
  CHECK(clock.advance(1.f) == 4u);
}

TEST_CASE("fixed step: steps cheaper than they simulate keep the full "
          "catch-up") {
  nx::fixed_step clock{{.max_steps = 4}};
  clock.record_cost(4, STEP);
  CHECK(clock.cap() == 4u);
  CHECK(clock.advance(1.f) == 4u);
}

TEST_CASE("fixed step: the cap is the steps whose cost fits in one step") {
  nx::fixed_step clock{{.max_steps = 4}};
  clock.record_cost(2, STEP / 1.25f);
  CHECK(clock.cap() == 2u);
  CHECK(clock.advance(STEP * 4.f) == 2u);
}

TEST_CASE("fixed step: a step costing more than it simulates runs once a "
          "frame and the rest is dropped") {
  nx::fixed_step clock{{.max_steps = 4}};
  clock.record_cost(1, STEP * 1.5f);
  CHECK(clock.cap() == 1u);
  const u64 dropped = clock.dropped();
  CHECK(clock.advance(STEP * 3.f) == 1u);
  CHECK(clock.dropped() > dropped);
}

TEST_CASE("fixed step: the cost is smoothed, so one slow step does not cut "
          "the cap and cheap steps win it back") {
  nx::fixed_step clock{{.max_steps = 4}};
  for (u32 i = 0; i < 20; ++i)
    clock.record_cost(1, STEP * 0.1f);
  clock.record_cost(1, STEP * 2.f);
  CHECK(clock.cap() > 1u);

  for (u32 i = 0; i < 20; ++i)
    clock.record_cost(1, STEP * 2.f);
  CHECK(clock.cap() == 1u);
  for (u32 i = 0; i < 40; ++i)
    clock.record_cost(1, STEP * 0.1f);
  CHECK(clock.cap() == 4u);
}

TEST_CASE("fixed step: an empty or free frame records no cost") {
  nx::fixed_step clock{{.max_steps = 4}};
  clock.record_cost(0, 1.f);
  clock.record_cost(3, 0.f);
  CHECK(clock.step_cost() == 0.f);
  CHECK(clock.cap() == 4u);
}

TEST_CASE("fixed step: a rate below once a second still steps") {
  nx::fixed_step clock{{.hz = 0.5f}};
  CHECK(clock.advance(1.f) == 0);
  CHECK(clock.advance(1.f) == 1);
  CHECK(clock.advance(10.f) == 1);
  CHECK(clock.alpha() == 0.f);
}
