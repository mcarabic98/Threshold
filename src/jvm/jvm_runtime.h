#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "jvm/jvm_env.h"
#include "jvm/vmstructs.h"
#include "memory/process.h"

namespace jvm {

struct JdkVersion {
  bool ok = false;
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string raw;     // e.g. "17.0.2", "1.8.0_312"
  std::string source;  // "release file" / "jvm.dll version resource"
};

// Version-agnostic HotSpot reader, entirely external:
//   1. locate jvm.dll inside the target process
//   2. find the VMStructs tables (anchor strings -> entry array walk)
//   3. derive all struct offsets / static addresses / int constants
// No injection, no signatures per JVM build — the tables describe themselves.
class JvmRuntime {
public:
  bool resolve(const mem::Process& proc);
  void reset();

  bool ok() const { return ok_; }
  const mem::ModuleInfo& module() const { return jvm_; }
  const VmStructsTable& structs() const { return structs_; }
  const JdkVersion& version() const { return version_; }
  const JvmEnv& env() const { return env_; }
  uint64_t structsTableStart() const { return tableStart_; }

  // JDK 21+ dropped Universe::_narrow_* from VMStructs; the heap reader
  // derives base/shift empirically (vmClasses + live ids) and installs it.
  void setNarrowKlass(uint64_t base, int shift) {
    env_.narrowKlassBase = base;
    env_.narrowKlassShift = shift;
    env_.compressedKlasses = true;
  }
  void setWideKlasses() { env_.compressedKlasses = false; }
  void setNarrowOop(uint64_t base, int shift) {
    env_.narrowOopBase = base;
    env_.narrowOopShift = shift;
    env_.compressedOops = true;
  }

  // HotSpot Symbol* (interned UTF-8 string) -> std::string.
  std::optional<std::string> readSymbol(const mem::Process& p, uint64_t symbol) const;

  // Address of vmClasses::_klasses[] (fundamental classes table in jvm.dll).
  std::optional<uint64_t> vmClassesArray() const;

  // Debug helper: log all VMStructs entries whose type/name matches filter.
  void dumpMatches(std::string_view filter, int maxLines = 80) const;

private:
  bool locateModule(const mem::Process& proc);
  bool parseStructsTable(const mem::Process& proc);
  bool parseIntConstants(const mem::Process& proc);
  void buildEnv(const mem::Process& proc);
  void detectVersion(const mem::Process& proc);

  mem::ModuleInfo jvm_;
  VmStructsTable structs_;
  JdkVersion version_;
  JvmEnv env_;
  uint64_t tableStart_ = 0;
  bool ok_ = false;
};

} // namespace jvm
