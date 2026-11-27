#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "jvm/jvm_runtime.h"
#include "memory/process.h"

namespace jvm {

struct FieldDesc {
  std::string name;
  std::string descriptor;
  uint32_t accessFlags = 0;
  uint32_t offset = 0; // instance: byte offset in object; static: offset in mirror
  bool isStatic = false;
};

// Reads java field tables (InstanceKlass::_fields) and constant-pool symbols
// externally. Field names resolve through ConstantPool::_handles (JDK 17+).
class KlassReader {
public:
  // Parse all fields (instance + static) of an InstanceKlass.
  bool readFields(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                  std::vector<FieldDesc>& out, std::string* err = nullptr);

  // Superclass InstanceKlass* (0 when there is none).
  std::optional<uint64_t> superClass(const mem::Process& p, const JvmRuntime& rt,
                                     uint64_t klass);

  // One-shot diagnostic: log everything needed to decode FieldInfo/CP layouts.
  void dumpClass(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                 const std::string& label);

private:
  // JDK <= 20 variant: _fields is a fixed-size Array<FieldInfo>; the entry
  // layout is detected from the data at runtime.
  bool readFieldsLegacy(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                        uint64_t fieldsArr, uint64_t cp, std::vector<FieldDesc>& out,
                        std::string* err);
};

} // namespace jvm
