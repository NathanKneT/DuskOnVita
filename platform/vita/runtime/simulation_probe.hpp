#pragma once
#include <cstdint>

namespace twv::vita {
// Serialized main-thread observations, independent of presentation count.
// Incremented only by the wrapper around the original fapGm_Execute call.
inline uint64_t simulationExecuteCalls=0;
}
