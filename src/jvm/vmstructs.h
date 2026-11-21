#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace jvm {

// Parsed HotSpot VMStructs tables (gHotSpotVMStructs equivalent).
// Non-static entries give field offsets inside HotSpot C++ structs;
// static entries give the address of the static datum in the JVM.
struct VmStructsTable {
  std::map<std::string, std::map<std::string, int64_t>> fields;   // non-static
  std::map<std::string, std::map<std::string, uint64_t>> statics; // static
  std::map<std::string, int64_t> intConstants;                    // "Klass::_lh_..."

  std::optional<int64_t> field(std::string_view type, std::string_view name) const;
  std::optional<uint64_t> staticDatum(std::string_view type, std::string_view name) const;
  std::optional<int64_t> intConstant(std::string_view name) const;
  size_t count() const;
  size_t constCount() const;
};

} // namespace jvm
