#include "core/util.h"

#include <windows.h>
#include <format>

namespace util {

std::string toUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string hex(uint64_t v) {
  return std::format("0x{:012X}", v);
}

std::string abbreviatePath(const std::string& utf8Path, size_t maxLen) {
  if (utf8Path.size() <= maxLen) return utf8Path;
  return "..." + utf8Path.substr(utf8Path.size() - (maxLen - 3));
}

} // namespace util
