#pragma once
#include <cstdint>
#include <string>

namespace util {

std::string toUtf8(const std::wstring& s);
std::wstring toWide(const std::string& s);

std::string hex(uint64_t v); // "0x00007FFA12345670"

std::string abbreviatePath(const std::string& utf8Path, size_t maxLen);

} // namespace util
