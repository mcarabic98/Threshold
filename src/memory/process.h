#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace mem {

struct ModuleInfo {
  std::wstring name;
  std::wstring path;
  uintptr_t base = 0;
  size_t size = 0;
};

// Read-mostly remote process wrapper.
//
// Reading is unrestricted and cheap. Writing goes through writeBuf(), which
// additionally requires write rights at attach time and logs every call with
// a reason — a feature must justify each write explicitly. The default
// attach mode is read-only.
class Process {
public:
  enum AccessFlags : DWORD {
    AccessRead  = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
    AccessWrite = PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
  };

  Process() = default;
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  bool attach(DWORD pid, bool withWrite);
  void detach();
  bool valid() const { return h_ != nullptr; }
  HANDLE handle() const { return h_; }
  bool canWrite() const { return withWrite_; }
  DWORD pid() const { return pid_; }

  // ---- reads ----
  bool readBuf(uintptr_t addr, void* out, size_t size) const;
  std::vector<uint8_t> readBytes(uintptr_t addr, size_t size) const;
  template <class T>
  bool read(uintptr_t addr, T* out) const {
    return readBuf(addr, out, sizeof(T));
  }
  template <class T>
  std::optional<T> read(uintptr_t addr) const {
    T v{};
    if (!readBuf(addr, &v, sizeof(T))) return std::nullopt;
    return v;
  }
  std::string readCStr(uintptr_t addr, size_t maxLen = 512) const;
  std::string readUtf16(uintptr_t addr, size_t maxChars = 512) const;

  // ---- write (gated) ----
  bool writeBuf(uintptr_t addr, const void* data, size_t size, const char* reason);

  // ---- modules / image ----
  std::vector<ModuleInfo> modules() const;
  std::optional<ModuleInfo> module(std::wstring_view name) const;
  std::wstring imagePath() const;

private:
  HANDLE h_ = nullptr;
  DWORD pid_ = 0;
  bool withWrite_ = false;
};

} // namespace mem
