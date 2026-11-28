#include "jvm/jvm_runtime.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <vector>

#include <windows.h>
#include <winver.h>

#include "core/util.h"

namespace jvm {

namespace {

constexpr size_t kMaxTableEntries = 200000;

// vmStructs.hpp VMStructEntry — stable layout from JDK 8 through 25
// (verified live on JDK 21): isStatic precedes offset/datum.
struct RemoteEntry {
  uint64_t typeName;
  uint64_t fieldName;
  uint64_t dataType;
  uint64_t isStatic;
  int64_t offset;
  uint64_t datum;
};
static_assert(sizeof(RemoteEntry) == 48, "unexpected VMStructEntry layout");

// VMStructs type names include template instantiations ("GrowableArray<int>"),
// so allow template/pointer/namespace characters besides plain identifiers.
bool identOk(const mem::Process& p, uint64_t addr, uint64_t modBase, uint64_t modEnd) {
  if (addr < modBase || addr >= modEnd) return false;
  char buf[80] = {};
  if (!p.readBuf(addr, buf, sizeof(buf))) return false;
  size_t n = strnlen(buf, sizeof(buf));
  if (n == 0 || n == sizeof(buf)) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)buf[i];
    if (!(std::isalnum(c) || c == '_' || c == '$' || c == ':' || c == '<' || c == '>' ||
          c == ',' || c == '*' || c == '.' || c == '[' || c == ']' || c == '(' || c == ')')) {
      return false;
    }
  }
  return true;
}

bool entryLooksValid(const mem::Process& p, const RemoteEntry& e, uint64_t modBase,
                     uint64_t modEnd) {
  if (e.isStatic > 1) return false;
  if (!identOk(p, e.typeName, modBase, modEnd)) return false;
  if (!identOk(p, e.fieldName, modBase, modEnd)) return false;
  if (e.isStatic) {
    if (e.datum == 0) return false;
  } else {
    if (e.offset < 0 || e.offset > 0x10000) return false;
  }
  return true;
}

// VMStructs int-constant names look like "Klass::_lh_log2_element_size_shift".
bool identColonOk(const mem::Process& p, uint64_t addr, uint64_t modBase, uint64_t modEnd) {
  if (addr < modBase || addr >= modEnd) return false;
  char buf[112] = {};
  if (!p.readBuf(addr, buf, sizeof(buf))) return false;
  size_t n = strnlen(buf, sizeof(buf));
  if (n == 0 || n == sizeof(buf)) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)buf[i];
    if (!(std::isalnum(c) || c == '_' || c == '$' || c == ':' || c == '<' || c == '>' ||
          c == ',' || c == '*' || c == ' ')) {
      return false;
    }
  }
  return true;
}

struct SectionSpan {
  uint64_t va;
  size_t size;
  std::string name;
};

// In-memory PE section ranges by name; works for 32- and 64-bit images.
std::vector<SectionSpan> sectionsByName(const mem::Process& p, const mem::ModuleInfo& mod,
                                        std::initializer_list<const char*> wanted) {
  std::vector<SectionSpan> out;
  IMAGE_DOS_HEADER dos{};
  if (!p.readBuf(mod.base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return out;
  const uint64_t nt = mod.base + (uint64_t)dos.e_lfanew;
  uint32_t sig = 0;
  uint16_t numSec = 0, optSize = 0;
  if (!p.read(nt, &sig) || sig != IMAGE_NT_SIGNATURE) return out;
  if (!p.read(nt + 6, &numSec)) return out;    // FileHeader.NumberOfSections
  if (!p.read(nt + 20, &optSize)) return out;  // FileHeader.SizeOfOptionalHeader
  const uint64_t secTable = nt + 24 + optSize;
  for (uint16_t i = 0; i < numSec && i < 96; ++i) {
    IMAGE_SECTION_HEADER sh{};
    if (!p.readBuf(secTable + (uint64_t)i * sizeof(sh), &sh, sizeof(sh))) break;
    char name[9] = {};
    std::memcpy(name, sh.Name, 8);
    for (const char* w : wanted) {
      if (_stricmp(name, w) == 0) {
        size_t sz = sh.Misc.VirtualSize ? (size_t)sh.Misc.VirtualSize
                                        : (size_t)sh.SizeOfRawData;
        if (sz) out.push_back({mod.base + sh.VirtualAddress, sz, name});
      }
    }
  }
  return out;
}

std::vector<uint8_t> readSection(const mem::Process& p, const SectionSpan& s) {
  std::vector<uint8_t> buf(s.size);
  if (buf.empty() || !p.readBuf(s.va, buf.data(), buf.size())) buf.clear();
  return buf;
}

// All addresses (VAs) where `s` occurs as a NUL-terminated string.
std::vector<uint64_t> findStringAddresses(const std::vector<uint8_t>& buf, uint64_t secVa,
                                          std::string_view s) {
  std::vector<uint64_t> out;
  if (s.size() + 1 > buf.size()) return out;
  for (size_t i = 0; i + s.size() + 1 <= buf.size(); ++i) {
    if (std::memcmp(buf.data() + i, s.data(), s.size()) == 0 && buf[i + s.size()] == 0) {
      out.push_back(secVa + i);
      i += s.size();
    }
  }
  return out;
}

using NamedSection = std::pair<SectionSpan, std::vector<uint8_t>>;

// Find 16 consecutive bytes [a][b] — the (typeName, fieldName) pair of one
// VMStructEntry. Scans 8-byte aligned positions across all section buffers.
std::optional<uint64_t> findPair(const std::vector<NamedSection>& secs, uint64_t a, uint64_t b) {
  for (auto& [span, buf] : secs) {
    if (buf.size() < 16) continue;
    for (size_t i = 0; i + 16 <= buf.size(); i += 8) {
      uint64_t x, y;
      std::memcpy(&x, buf.data() + i, 8);
      if (x != a) continue;
      std::memcpy(&y, buf.data() + i + 8, 8);
      if (y == b) return span.va + i;
    }
  }
  return std::nullopt;
}

std::optional<std::string> queryFileVersion(const std::wstring& path) {
  DWORD handle = 0;
  DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!sz) return std::nullopt;
  std::vector<char> data(sz);
  if (!GetFileVersionInfoW(path.c_str(), 0, sz, data.data())) return std::nullopt;
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", (LPVOID*)&fi, &len) || !fi) return std::nullopt;
  return std::format("{}.{}.{}.{}", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                     HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}

void parseVersion(JdkVersion& v) {
  // "1.8.0_312" / "17.0.2" / "21.0.5+11-LTS" / "25"
  std::string s;
  for (char c : v.raw) {
    if (std::isdigit((unsigned char)c) || c == '.' || c == '_')
      s += c;
    else
      break;
  }
  int nums[4] = {0, 0, 0, 0};
  int idx = 0;
  for (size_t i = 0; i <= s.size() && idx < 4; ++i) {
    if (i == s.size() || s[i] == '.' || s[i] == '_') {
      ++idx;
      continue;
    }
    nums[idx] = nums[idx] * 10 + (s[i] - '0');
  }
  if (nums[0] == 1) { // legacy "1.8.0_x"
    v.major = nums[1];
    v.minor = nums[2];
    v.patch = nums[3];
  } else {
    v.major = nums[0];
    v.minor = nums[1];
    v.patch = nums[2];
  }
}

} // namespace

bool JvmRuntime::resolve(const mem::Process& proc) {
  ok_ = false;
  structs_ = {};
  version_ = {};
  env_ = {};
  tableStart_ = 0;

  if (!locateModule(proc)) return false;
  if (!parseStructsTable(proc)) return false;
  parseIntConstants(proc);
  buildEnv(proc);
  detectVersion(proc);

  spdlog::info(
      "jvm: jvm.dll at {} ({} MB), {} VMStructs + {} int constants, JDK '{}' ({})",
      util::hex(jvm_.base), jvm_.size / (1024 * 1024), structs_.count(),
      structs_.constCount(), version_.raw.empty() ? "?" : version_.raw,
      version_.ok ? version_.source : "unrecognized");

  ok_ = true;
  return true;
}

void JvmRuntime::reset() {
  ok_ = false;
  structs_ = {};
  version_ = {};
  env_ = {};
  jvm_ = {};
  tableStart_ = 0;
}

bool JvmRuntime::locateModule(const mem::Process& proc) {
  auto m = proc.module(L"jvm.dll");
  if (!m) {
    spdlog::debug("jvm: no jvm.dll loaded — not a HotSpot process");
    return false;
  }
  jvm_ = *m;
  return true;
}

bool JvmRuntime::parseStructsTable(const mem::Process& proc) {
  const uint64_t base = jvm_.base;
  const uint64_t end = base + jvm_.size;

  std::vector<NamedSection> secs;
  for (auto& s : sectionsByName(proc, jvm_, {".rdata", ".data"})) {
    auto buf = readSection(proc, s);
    if (!buf.empty()) secs.emplace_back(s, std::move(buf));
  }
  if (secs.empty()) {
    spdlog::warn("jvm: failed to read jvm.dll sections");
    return false;
  }

  // Anchor strings: type name + field name that only appear as the first
  // members of one VMStructEntry each.
  std::vector<uint64_t> a1, b1, a2, b2;
  for (auto& [span, buf] : secs) {
    for (auto& a : findStringAddresses(buf, span.va, "Klass")) a1.push_back(a);
    for (auto& a : findStringAddresses(buf, span.va, "_layout_helper")) b1.push_back(a);
    for (auto& a : findStringAddresses(buf, span.va, "oopDesc")) a2.push_back(a);
    for (auto& a : findStringAddresses(buf, span.va, "_mark")) b2.push_back(a);
  }

  std::optional<uint64_t> entryAddr;
  for (uint64_t a : a1)
    for (uint64_t b : b1) {
      entryAddr = findPair(secs, a, b);
      if (entryAddr) break;
    }
  if (!entryAddr)
    for (uint64_t a : a2)
      for (uint64_t b : b2) {
        entryAddr = findPair(secs, a, b);
        if (entryAddr) break;
      }
  if (!entryAddr) {
    spdlog::warn("jvm: VMStructs anchor not found (unsupported/obfuscated JVM?)");
    return false;
  }

  // Validate the anchor entry, then walk back to the table start.
  RemoteEntry e{};
  uint64_t start = *entryAddr;
  if (!proc.readBuf(start, &e, sizeof(e)) || !entryLooksValid(proc, e, base, end)) {
    spdlog::warn("jvm: anchor entry at {} failed validation", util::hex(start));
    return false;
  }
  for (size_t guard = 0; guard < kMaxTableEntries; ++guard) {
    uint64_t cand = start - sizeof(RemoteEntry);
    if (cand < base) break;
    if (!proc.readBuf(cand, &e, sizeof(e)) || !entryLooksValid(proc, e, base, end)) break;
    start = cand;
  }

  // Walk forward to the terminator, collecting entries.
  std::vector<RemoteEntry> entries;
  entries.reserve(4096);
  uint64_t cur = start;
  while (entries.size() < kMaxTableEntries) {
    if (!proc.readBuf(cur, &e, sizeof(e))) break;
    if (e.typeName == 0 && e.fieldName == 0) break; // terminator entry
    if (!entryLooksValid(proc, e, base, end)) {
      auto tn = proc.readCStr(e.typeName, 64);
      auto fn = proc.readCStr(e.fieldName, 64);
      auto dt = proc.readCStr(e.dataType, 48);
      spdlog::debug(
          "jvm: walk stopped at entry {}: type='{}' field='{}' dataType='{}' datum=0x{:X} "
          "offset={} isStatic={}",
          entries.size(), tn, fn, dt, e.datum, e.offset, e.isStatic);
      break;
    }
    entries.push_back(e);
    cur += sizeof(RemoteEntry);
  }
  if (entries.size() < 100) {
    spdlog::warn("jvm: implausibly small VMStructs table ({} entries)", entries.size());
    return false;
  }

  for (auto& en : entries) {
    std::string type = proc.readCStr(en.typeName, 64);
    std::string field = proc.readCStr(en.fieldName, 64);
    if (type.empty() || field.empty()) continue;
    if (en.isStatic)
      structs_.statics[type][field] = en.datum;
    else
      structs_.fields[type][field] = en.offset;
  }

  // The layout is only trusted if a few well-known keys survived.
  bool good = structs_.field("oopDesc", "_mark").has_value() &&
              structs_.field("Klass", "_layout_helper").has_value() &&
              structs_.field("Symbol", "_length").has_value() &&
              structs_.field("Symbol", "_body").has_value();
  if (!good) {
    spdlog::warn("jvm: VMStructs parsed ({} entries) but expected keys missing", entries.size());
    return false;
  }

  tableStart_ = start;

  if (auto v = structs_.field("Klass", "_layout_helper"))
    spdlog::debug("  Klass::_layout_helper = {}", *v);
  if (auto v = structs_.field("InstanceKlass", "_fields"))
    spdlog::debug("  InstanceKlass::_fields = {}", *v);
  if (auto v = structs_.staticDatum("Universe", "_collectedHeap"))
    spdlog::debug("  Universe::_collectedHeap @ {}", util::hex(*v));
  if (auto v = structs_.staticDatum("Universe", "_narrow_oop_base"))
    spdlog::debug("  Universe::_narrow_oop_base @ {}", util::hex(*v));

  return true;
}

void JvmRuntime::detectVersion(const mem::Process& proc) {
  auto img = proc.imagePath();
  if (img.empty()) return;
  std::filesystem::path dir = std::filesystem::path(img).parent_path();
  for (int i = 0; i < 6 && !dir.empty(); ++i) {
    std::error_code ec;
    auto rel = dir / L"release";
    if (std::filesystem::exists(rel, ec)) {
      std::ifstream f(rel);
      std::string line;
      while (std::getline(f, line)) {
        if (line.rfind("JAVA_VERSION=", 0) == 0) {
          std::string v = line.substr(13);
          v.erase(std::remove_if(v.begin(), v.end(),
                                 [](char c) { return c == '"' || c == '\r' || c == ' '; }),
                  v.end());
          version_.raw = v;
          parseVersion(version_);
          version_.ok = version_.major > 0;
          version_.source = "release file";
          return;
        }
      }
    }
    auto parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  if (auto v = queryFileVersion(jvm_.path)) {
    version_.raw = *v;
    parseVersion(version_);
    version_.ok = version_.major > 0;
    version_.source = "jvm.dll version resource";
  }
}

std::optional<std::string> JvmRuntime::readSymbol(const mem::Process& p, uint64_t symbol) const {
  auto lenOff = structs_.field("Symbol", "_length");
  auto bodyOff = structs_.field("Symbol", "_body");
  if (!lenOff || !bodyOff) return std::nullopt;
  uint16_t len = 0;
  if (!p.read(symbol + (uint64_t)*lenOff, &len)) return std::nullopt;
  if (len == 0 || len > 1024) return std::nullopt;
  std::string s(len, '\0');
  if (!p.readBuf(symbol + (uint64_t)*bodyOff, s.data(), len)) return std::nullopt;
  return s;
}

std::optional<uint64_t> JvmRuntime::vmClassesArray() const {
  if (auto it = structs_.statics.find("vmClasses"); it != structs_.statics.end())
    for (auto& [f, v] : it->second)
      if (f.rfind("_klasses[", 0) == 0) return v;
  return std::nullopt;
}

// gHotSpotVMIntConstants: VMIntConstantEntry { const char* name; int value; }
bool JvmRuntime::parseIntConstants(const mem::Process& proc) {
  const uint64_t base = jvm_.base;
  const uint64_t end = base + jvm_.size;

  std::vector<NamedSection> secs;
  for (auto& s : sectionsByName(proc, jvm_, {".rdata", ".data"})) {
    auto buf = readSection(proc, s);
    if (!buf.empty()) secs.emplace_back(s, std::move(buf));
  }

  // Anchor: the name of a well-known layout-helper constant.
  std::vector<uint64_t> anchorStrs;
  for (auto& [span, buf] : secs)
    for (auto& a : findStringAddresses(buf, span.va, "Klass::_lh_log2_element_size_shift"))
      anchorStrs.push_back(a);

  std::optional<uint64_t> entryAddr;
  for (auto& [span, buf] : secs) {
    if (buf.size() < 8) continue;
    for (uint64_t a : anchorStrs) {
      for (size_t i = 0; i + 8 <= buf.size(); i += 8) {
        uint64_t x;
        std::memcpy(&x, buf.data() + i, 8);
        if (x == a) {
          entryAddr = span.va + i;
          break;
        }
      }
      if (entryAddr) break;
    }
    if (entryAddr) break;
  }
  if (!entryAddr) {
    spdlog::warn("jvm: VM int constants table not found");
    return false;
  }

  uint64_t start = *entryAddr;
  for (size_t guard = 0; guard < kMaxTableEntries; ++guard) {
    uint64_t cand = start - 16;
    if (cand < base) break;
    auto name = proc.read<uint64_t>((uintptr_t)cand);
    if (!name || !identColonOk(proc, *name, base, end)) break;
    start = cand;
  }

  size_t count = 0;
  uint64_t cur = start;
  while (count < kMaxTableEntries) {
    auto name = proc.read<uint64_t>((uintptr_t)cur);
    if (!name || *name == 0) break;
    if (!identColonOk(proc, *name, base, end)) break;
    auto val = proc.read<int32_t>((uintptr_t)(cur + 8));
    if (!val) break;
    structs_.intConstants[proc.readCStr(*name, 96)] = *val;
    cur += 16;
    ++count;
  }
  if (count < 10) {
    spdlog::warn("jvm: implausibly small int constants table ({} entries)", count);
    return false;
  }
  spdlog::debug("jvm: {} VM int constants parsed", count);
  return true;
}

void JvmRuntime::buildEnv(const mem::Process& proc) {
  JvmEnv e;
  auto getStaticU64 = [&](const char* t, const char* f, uint64_t& out) {
    if (auto d = structs_.staticDatum(t, f))
      if (auto v = proc.read<uint64_t>((uintptr_t)*d)) {
        out = *v;
        return true;
      }
    return false;
  };
  auto getStaticInt = [&](const char* t, const char* f, int& out) {
    if (auto d = structs_.staticDatum(t, f))
      if (auto v = proc.read<int32_t>((uintptr_t)*d)) {
        out = *v;
        return true;
      }
    return false;
  };

  // Heap data oops: compressed unless explicitly disabled — JDK 21+ dropped
  // the Universe::_narrow_oop_* statics entirely, so missing statics must not
  // flip the flag. base/shift stay best-effort: when absent (0/0), the model
  // derives the real decode from hypotheses over live objects.
  getStaticU64("Universe", "_narrow_oop_base", e.narrowOopBase) ||
      getStaticU64("Universe", "_narrow_oop._base", e.narrowOopBase);
  getStaticInt("Universe", "_narrow_oop_shift", e.narrowOopShift) ||
      getStaticInt("Universe", "_narrow_oop._shift", e.narrowOopShift);
  e.compressedOops = true;

  // Compressed class pointers: flat names on JDK <= 16, nested
  // "Universe::_narrow_klass._*" on 17-20, CompressedKlassPointers on 21+.
  bool klBase = getStaticU64("Universe", "_narrow_klass_base", e.narrowKlassBase) ||
                getStaticU64("Universe", "_narrow_klass._base", e.narrowKlassBase) ||
                getStaticU64("CompressedKlassPointers", "_encoded_base", e.narrowKlassBase);
  bool klShift = getStaticInt("Universe", "_narrow_klass_shift", e.narrowKlassShift) ||
                 getStaticInt("Universe", "_narrow_klass._shift", e.narrowKlassShift) ||
                 getStaticInt("CompressedKlassPointers", "_encoded_shift", e.narrowKlassShift);
  e.compressedKlasses = klBase && klShift;
  if (!e.compressedKlasses) {
    e.narrowKlassBase = 0;
    e.narrowKlassShift = 0;
    spdlog::debug("jvm: narrow klass statics not found; candidates:");
    dumpMatches("narrow", 20);
    dumpMatches("klass", 30);
    dumpMatches("Universe", 60);
  }

  if (auto v = structs_.field("Klass", "_name")) e.offKlassName = *v;
  if (auto v = structs_.field("Klass", "_layout_helper")) e.offKlassLayoutHelper = *v;
  if (auto v = structs_.field("InstanceKlass", "_java_mirror")) e.offKlassMirror = *v;
  if (auto v = structs_.field("Symbol", "_length")) e.offSymbolLength = *v;
  if (auto v = structs_.field("Symbol", "_body")) e.offSymbolBody = *v;

  auto ic = [&](const char* name, int& out) {
    if (auto v = structs_.intConstant(name)) {
      out = (int)*v;
      return true;
    }
    return false;
  };
  // "_lh_array_tag_oop_value" was renamed "_lh_array_tag_obj_value" in JDK 21.
  bool tagOop = ic("Klass::_lh_array_tag_oop_value", e.lhArrayTagOopVal) ||
                ic("Klass::_lh_array_tag_obj_value", e.lhArrayTagOopVal);
  bool lhOk = ic("Klass::_lh_instance_slow_path_bit", e.lhInstanceSlowPathBit) &&
              ic("Klass::_lh_log2_element_size_shift", e.lhLog2ElemShift) &&
              ic("Klass::_lh_log2_element_size_mask", e.lhLog2ElemMask) &&
              ic("Klass::_lh_array_tag_shift", e.lhArrayTagShift) &&
              ic("Klass::_lh_array_tag_type_value", e.lhArrayTagTypeVal) && tagOop;
  // Header-size constants (JDK 9+): optional; arrayHeaderSize() falls back.
  bool hdrOk = ic("Klass::_lh_header_size_shift", e.lhHeaderSizeShift) &&
               ic("Klass::_lh_header_size_mask", e.lhHeaderSizeMask);
  if (!hdrOk) {
    e.lhHeaderSizeShift = 0;
    e.lhHeaderSizeMask = 0;
  }

  e.valid = e.offKlassName >= 0 && e.offKlassLayoutHelper >= 0 && e.offSymbolLength >= 0 &&
            e.offSymbolBody >= 0 && lhOk;
  if (!lhOk) {
    spdlog::warn("jvm: some layout-helper int constants missing; available:");
    for (auto& [k, v] : structs_.intConstants)
      if (k.rfind("Klass::_lh", 0) == 0) spdlog::warn("  {} = {}", k, v);
  }
  if (!e.valid)
    spdlog::warn(
        "jvm: env incomplete (name={} lh={} sym={} lhOk={} compressedKlasses={})",
        e.offKlassName, e.offKlassLayoutHelper, e.offSymbolLength >= 0, lhOk,
        e.compressedKlasses);

  spdlog::debug(
      "  env: narrowOop base={} shift={}, narrowKlass base={} shift={}",
      util::hex(e.narrowOopBase), e.narrowOopShift, util::hex(e.narrowKlassBase),
      e.narrowKlassShift);
  env_ = e;
}

void JvmRuntime::dumpMatches(std::string_view filter, int maxLines) const {
  int n = 0;
  for (auto& [type, m] : structs_.fields) {
    if (type.find(filter) == std::string::npos) continue;
    for (auto& [f, v] : m) {
      spdlog::debug("  dump {}.{} = {}", type, f, v);
      if (++n >= maxLines) {
        spdlog::debug("  dump ... truncated");
        return;
      }
    }
  }
  for (auto& [type, m] : structs_.statics) {
    if (type.find(filter) == std::string::npos) continue;
    for (auto& [f, v] : m) {
      spdlog::debug("  dump {}::{} @ {}", type, f, util::hex(v));
      if (++n >= maxLines) {
        spdlog::debug("  dump ... truncated");
        return;
      }
    }
  }
  for (auto& [k, v] : structs_.intConstants) {
    if (k.find(filter) == std::string::npos) continue;
    spdlog::debug("  dump {} = {}", k, v);
    if (++n >= maxLines) {
      spdlog::debug("  dump ... truncated");
      return;
    }
  }
}

} // namespace jvm
