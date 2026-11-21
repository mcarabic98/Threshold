#include "memory/process.h"

#include <spdlog/spdlog.h>
#include <psapi.h>

#include <algorithm>

#include "core/util.h"

namespace mem {

Process::~Process() {
  detach();
}

bool Process::attach(DWORD pid, bool withWrite) {
  detach();
  DWORD access = AccessRead | (withWrite ? AccessWrite : 0);
  HANDLE h = OpenProcess(access, FALSE, pid);
  if (!h) {
    spdlog::error("OpenProcess(pid={}) failed, gle={}", pid, GetLastError());
    return false;
  }
  h_ = h;
  pid_ = pid;
  withWrite_ = withWrite;
  return true;
}

void Process::detach() {
  if (h_) {
    CloseHandle(h_);
    h_ = nullptr;
  }
  pid_ = 0;
  withWrite_ = false;
}

bool Process::readBuf(uintptr_t addr, void* out, size_t size) const {
  if (!valid()) return false;
  if (size == 0) return true;
  auto* dst = (uint8_t*)out;
  size_t done = 0;
  while (done < size) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(h_, (LPCVOID)(addr + done), dst + done, size - done, &got))
      return false;
    if (got == 0) return false;
    done += got;
  }
  return true;
}

std::vector<uint8_t> Process::readBytes(uintptr_t addr, size_t size) const {
  std::vector<uint8_t> buf(size);
  if (size == 0 || !readBuf(addr, buf.data(), size)) return {};
  return buf;
}

std::string Process::readCStr(uintptr_t addr, size_t maxLen) const {
  auto bytes = readBytes(addr, maxLen);
  std::string s((const char*)bytes.data(), bytes.size());
  auto pos = s.find('\0');
  if (pos != std::string::npos) s.resize(pos);
  return s;
}

std::string Process::readUtf16(uintptr_t addr, size_t maxChars) const {
  auto bytes = readBytes(addr, maxChars * 2);
  std::wstring w(bytes.size() / 2, L'\0');
  std::memcpy(w.data(), bytes.data(), bytes.size());
  auto pos = w.find(L'\0');
  if (pos != std::wstring::npos) w.resize(pos);
  return util::toUtf8(w);
}

bool Process::writeBuf(uintptr_t addr, const void* data, size_t size, const char* reason) {
  if (!valid()) return false;
  if (!withWrite_) {
    spdlog::error("WRITE denied (process opened read-only): addr={} size={} reason='{}'",
                  util::hex(addr), size, reason);
    return false;
  }
  SIZE_T written = 0;
  BOOL ok = WriteProcessMemory(h_, (LPVOID)addr, data, size, &written);
  spdlog::warn("WRITE pid={} addr={} size={} reason='{}' -> {}", pid_, util::hex(addr),
               size, reason, (ok && written == size) ? "ok" : "FAILED");
  return ok && written == size;
}

std::vector<ModuleInfo> Process::modules() const {
  std::vector<ModuleInfo> out;
  if (!valid()) return out;
  HMODULE mods[1024];
  DWORD needed = 0;
  if (!EnumProcessModulesEx(h_, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return out;
  int count = (int)std::min<size_t>(needed / sizeof(HMODULE), 1024);
  for (int i = 0; i < count; ++i) {
    MODULEINFO mi{};
    if (!GetModuleInformation(h_, mods[i], &mi, sizeof(mi))) continue;
    wchar_t name[MAX_PATH] = {};
    wchar_t path[1024] = {};
    GetModuleBaseNameW(h_, mods[i], name, MAX_PATH);
    GetModuleFileNameExW(h_, mods[i], path, 1024);
    out.push_back({name, path, (uintptr_t)mi.lpBaseOfDll, (size_t)mi.SizeOfImage});
  }
  return out;
}

std::optional<ModuleInfo> Process::module(std::wstring_view name) const {
  for (auto& m : modules())
    if (_wcsicmp(m.name.c_str(), name.data()) == 0) return m;
  return std::nullopt;
}

std::wstring Process::imagePath() const {
  wchar_t buf[1024] = {};
  DWORD n = 1024;
  if (!QueryFullProcessImageNameW(h_, 0, buf, &n)) return {};
  return std::wstring(buf, n);
}

} // namespace mem
