#include "mc/finder.h"

#include <psapi.h>

#include <algorithm>
#include <spdlog/spdlog.h>

namespace mc {

namespace {

struct ScanCtx {
  std::vector<McCandidate>* out;
  DWORD selfPid;
};

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
  auto* ctx = (ScanCtx*)lp;
  if (!IsWindowVisible(hwnd)) return TRUE;
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (style & WS_CHILD) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == ctx->selfPid) return TRUE;
  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);
  if (title[0] == L'\0') return TRUE;

  McCandidate c;
  c.hwnd = hwnd;
  c.pid = pid;
  c.title = title;

  HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (ph) {
    BOOL wow64 = FALSE;
    IsWow64Process(ph, &wow64);
    c.is64Bit = !wow64;
    wchar_t path[1024] = {};
    DWORD n = 1024;
    QueryFullProcessImageNameW(ph, 0, path, &n);
    c.processPath = path;

    // jvm.dll detection only works for same-bitness processes.
    HMODULE mods[1024];
    DWORD needed = 0;
    if (c.is64Bit && EnumProcessModulesEx(ph, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
      int cnt = (int)std::min<size_t>(needed / sizeof(HMODULE), 1024);
      for (int i = 0; i < cnt; ++i) {
        wchar_t base[MAX_PATH] = {};
        GetModuleBaseNameW(ph, mods[i], base, MAX_PATH);
        if (_wcsicmp(base, L"jvm.dll") == 0) {
          c.hasJvm = true;
          break;
        }
      }
    }
    CloseHandle(ph);
  }
  ctx->out->push_back(c);
  return TRUE;
}

} // namespace

std::vector<McCandidate> findCandidates() {
  std::vector<McCandidate> out;
  ScanCtx ctx{&out, GetCurrentProcessId()};
  EnumWindows(enumProc, (LPARAM)&ctx);
  return out;
}

std::optional<McCandidate> pickBest(const std::vector<McCandidate>& cands) {
  const McCandidate* best = nullptr;
  for (auto& c : cands)
    if (c.hasJvm && c.is64Bit && !best) best = &c;
  if (!best) return std::nullopt;
  return *best;
}

} // namespace mc
