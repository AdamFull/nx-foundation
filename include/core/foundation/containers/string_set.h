#pragma once

#include "core/foundation/containers/hash_set.h"
#include "core/foundation/containers/string_map.h"

namespace nx {

using string_set = flat_hash_set<string, string_hash, string_equal>;

}
