
#include "framework/nxtest.h"

#include "core/foundation/reflection/registry.h"

namespace nxtest_attr {

struct Body {
  f32 gravity_scale = 1.f;
  f32 damping = 0.f;
  bool interpolating = false;
  bool moved = false;
  i32 plain = 0;
};

struct Untouched {
  f32 a = 0.f;
  f32 b = 0.f;
};

}

using nx::reflect::field_flag;

NX_ATTRIBUTES(nxtest_attr::Body,
              (gravity_scale, .min = 0.0, .max = 10.0,
               NX_EDITOR_ONLY(.label = "Gravity")),
              (interpolating,
               .flags = field_flag::transient | field_flag::read_only),
              (moved, .flags = field_flag::transient))

namespace {

using namespace nxtest_attr;

[[nodiscard]] const nx::reflect::field_info &named(const nx::string_view n) {
  const auto *f = nx::reflect::type_of<Body>().field(n);
  REQUIRE(f != nullptr);
  return *f;
}

}

TEST_CASE("attributes: a field says nothing unless it means to") {
  const auto &f = named("damping");
  CHECK(f.attributes.flags.none());
  CHECK(f.attributes.min == f.attributes.max);
  CHECK(f.serialised());

  const auto *a = nx::reflect::type_of<Untouched>().field("a");
  REQUIRE(a != nullptr);
  CHECK(a->attributes.flags.none());
  CHECK(a->serialised());
}

TEST_CASE("attributes: an attributed field carries what was said") {
  const auto &f = named("gravity_scale");
  CHECK(f.attributes.min == 0.0);
  CHECK(f.attributes.max == 10.0);
  CHECK(f.attributes.flags.none());
  CHECK(f.serialised());
#if NX_REFLECT_EDITOR
  CHECK(f.attributes.label == "Gravity");
#endif
}

TEST_CASE("attributes: transient is described, which absent is not") {
  const auto &f = named("interpolating");
  CHECK_FALSE(f.serialised());
  CHECK(f.has(field_flag::transient));
  CHECK(f.has(field_flag::read_only));
  CHECK_FALSE(f.has(field_flag::hidden));

  CHECK(nx::reflect::type_of<Body>().field("interpolating") != nullptr);
  CHECK(nx::reflect::type_of<Body>().field("no_such_field") == nullptr);
}

TEST_CASE("attributes: discovery still finds every field, attributed or not") {
  CHECK(nx::reflect::type_of<Body>().fields.size() == 5u);
}

TEST_CASE("registry: a type is findable by the name a document would use") {
  auto &registry = nx::reflect::type_registry::instance();
  registry.add<Body>();

  const auto *by_name = registry.find("nxtest_attr::Body");
  REQUIRE(by_name != nullptr);
  CHECK(by_name == &nx::reflect::type_of<Body>());

  const auto *by_id = registry.find(nx::reflect::type_of<Body>().id);
  CHECK(by_id == by_name);
}

TEST_CASE("registry: nothing is findable by accident") {
  auto &registry = nx::reflect::type_registry::instance();
  CHECK(nx::reflect::type_of<Untouched>().fields.size() == 2u);
  CHECK(registry.find("nxtest_attr::Untouched") == nullptr);
  CHECK(registry.find("nothing::at::all") == nullptr);
}

TEST_CASE("registry: registering twice is not an error") {
  auto &registry = nx::reflect::type_registry::instance();
  registry.add<Body>();
  const usize before = registry.size();
  registry.add<Body>();
  CHECK(registry.size() == before);
}
