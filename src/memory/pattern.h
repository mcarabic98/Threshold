#pragma once
#include <cstdint>
#include <string_view>

namespace mem {

// IDA-style pattern: "48 8B 05 ? ? ? ? 48 85 C0"
// Case-insensitive hex nibbles; '?' or '??' are wildcards.
// Returns match offset within data, or (size_t)-1.
size_t findPattern(const uint8_t* data, size_t size, std::string_view pattern);

// RIP-relative operand resolution (pure math, no reads):
//   target = (address of displacement) + dispBytes + instrTailAfterDisp + disp32
uintptr_t resolveRipRel(uintptr_t matchAddr, size_t dispPosInMatch, int32_t disp,
                        size_t dispBytes = 4, size_t instrTailAfterDisp = 0);

} // namespace mem
