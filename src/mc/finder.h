#pragma once
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace mc {

struct McCandidate {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::wstring title;
  std::wstring processPath;
  bool hasJvm = false;
  bool is64Bit = true;
};

// All visible top-level windows; each annotated with process info and
// whether its process has jvm.dll loaded (i.e. is a HotSpot JVM = Java game).
std::vector<McCandidate> findCandidates();

// First 64-bit candidate with jvm.dll.
std::optional<McCandidate> pickBest(const std::vector<McCandidate>& cands);

} // namespace mc
