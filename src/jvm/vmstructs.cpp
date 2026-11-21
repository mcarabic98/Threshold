#include "jvm/vmstructs.h"

namespace jvm {

std::optional<int64_t> VmStructsTable::field(std::string_view type, std::string_view name) const {
  auto it = fields.find(std::string(type));
  if (it == fields.end()) return std::nullopt;
  auto it2 = it->second.find(std::string(name));
  if (it2 == it->second.end()) return std::nullopt;
  return it2->second;
}

std::optional<uint64_t> VmStructsTable::staticDatum(std::string_view type,
                                                    std::string_view name) const {
  auto it = statics.find(std::string(type));
  if (it == statics.end()) return std::nullopt;
  auto it2 = it->second.find(std::string(name));
  if (it2 == it->second.end()) return std::nullopt;
  return it2->second;
}

std::optional<int64_t> VmStructsTable::intConstant(std::string_view name) const {
  auto it = intConstants.find(std::string(name));
  if (it == intConstants.end()) return std::nullopt;
  return it->second;
}

size_t VmStructsTable::count() const {
  size_t n = 0;
  for (auto& [_, m] : fields) n += m.size();
  for (auto& [_, m] : statics) n += m.size();
  return n;
}

size_t VmStructsTable::constCount() const {
  return intConstants.size();
}

} // namespace jvm
