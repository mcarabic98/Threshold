#include "memory/pattern.h"

#include <vector>

namespace mem {

namespace {

struct Parsed {
  std::vector<uint8_t> bytes;
  std::vector<bool> mask;
};

Parsed parse(std::string_view pattern) {
  Parsed p;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < pattern.size();) {
    char c = pattern[i];
    if (c == ' ') {
      ++i;
      continue;
    }
    if (c == '?') {
      p.bytes.push_back(0);
      p.mask.push_back(false);
      ++i;
      if (i < pattern.size() && pattern[i] == '?') ++i;
      continue;
    }
    int hi = nib(c);
    if (hi < 0) {
      ++i;
      continue;
    }
    int lo = -1;
    if (i + 1 < pattern.size() && nib(pattern[i + 1]) >= 0) {
      lo = nib(pattern[i + 1]);
      i += 2;
    } else {
      i += 1;
    }
    p.bytes.push_back((uint8_t)((hi << 4) | (lo < 0 ? 0 : lo)));
    p.mask.push_back(true);
  }
  return p;
}

} // namespace

size_t findPattern(const uint8_t* data, size_t size, std::string_view pattern) {
  auto p = parse(pattern);
  if (p.bytes.empty() || size < p.bytes.size()) return (size_t)-1;
  const size_t last = size - p.bytes.size();
  for (size_t i = 0; i <= last; ++i) {
    bool ok = true;
    for (size_t j = 0; j < p.bytes.size(); ++j) {
      if (p.mask[j] && data[i + j] != p.bytes[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return i;
  }
  return (size_t)-1;
}

uintptr_t resolveRipRel(uintptr_t matchAddr, size_t dispPosInMatch, int32_t disp,
                        size_t dispBytes, size_t instrTailAfterDisp) {
  return matchAddr + dispPosInMatch + dispBytes + instrTailAfterDisp + (uintptr_t)(int64_t)disp;
}

} // namespace mem
