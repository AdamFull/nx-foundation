#pragma once

#include "core/foundation/serialization/json.h"
#include "core/foundation/serialization/nva.h"

namespace nx::json {

void write_binary(const value &v, nx::nva::Writer &w);

[[nodiscard]] value read_binary(nx::nva::Reader &r);

}
