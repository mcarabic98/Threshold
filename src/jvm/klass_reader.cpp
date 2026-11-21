#include "jvm/klass_reader.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <format>

#include "core/util.h"

namespace jvm {

namespace {

struct ArrayOffsets {
  int64_t length = -1;
  int64_t data = -1;
};

// HotSpot Array<T> layout is instantiation-independent; VMStructs describes a
// few of them, any instantiation's offsets work for all.
ArrayOffsets arrayOffsets(const VmStructsTable& st) {
  ArrayOffsets o;
  for (const char* tn : {"Array<int>", "Array<u1>", "Array<u2>", "Array<u4>",
                         "Array<OopHandle>", "Array<FieldInfo>", "Array<Method*>"}) {
    auto l = st.field(tn, "_length");
    auto d = st.field(tn, "_data");
    if (l && d) {
      o.length = *l;
      o.data = *d;
      break;
    }
  }
  return o;
}

std::string hexDump(const uint8_t* data, size_t n) {
  std::string out;
  char tmp[8];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(tmp, sizeof(tmp), "%02x", data[i]);
    out += tmp;
    if ((i & 7) == 7) out += ' ';
  }
  return out;
}

// Pack200-style UNSIGNED5 varint (utilities/unsigned5.hpp):
//   byte 0        -> excluded (End marker)
//   bytes 1..191  -> terminal low byte, contributes value
//   bytes 192..255-> high byte, contributes (b-1) << 6*i
struct U5Reader {
  const uint8_t* data;
  size_t len;
  size_t pos = 0;

  uint32_t next() {
    uint32_t b0 = data[pos++];
    uint32_t sum = b0 - 1;
    if (sum < 192) return sum;
    for (int i = 1;; ++i) {
      uint32_t bi = data[pos++];
      sum += (bi - 1) << (6 * i);
      if (bi < 193 || i == 4) return sum;
    }
  }
  bool hasEnd() { return pos < len && data[pos] == 0; }
};

// Field flags (FieldInfo::FieldFlags bit positions).
enum : uint32_t {
  FF_INITIALIZED = 1 << 0,
  FF_INJECTED = 1 << 1,
  FF_GENERIC = 1 << 2,
  FF_STABLE = 1 << 3,
  FF_CONTENDED = 1 << 4,
};

} // namespace

// JVM ACC flags for fields.
static constexpr uint32_t ACC_STATIC = 0x0008;

bool KlassReader::readFields(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                             std::vector<FieldDesc>& out, std::string* err) {
  out.clear();
  auto& st = rt.structs();
  const JvmEnv& env = rt.env();

  auto fiOff = st.field("InstanceKlass", "_fieldinfo_stream");
  auto cOff = st.field("InstanceKlass", "_constants");
  if (!fiOff || !cOff) {
    if (err) *err = "VMStructs missing _fieldinfo_stream/_constants";
    return false;
  }
  auto fiArr = p.read<uint64_t>((uintptr_t)(klass + *fiOff));
  auto cpOpt = p.read<uint64_t>((uintptr_t)(klass + *cOff));
  if (!fiArr || !*fiArr || !cpOpt || !*cpOpt) {
    if (err) *err = "unreadable _fieldinfo_stream/_constants";
    return false;
  }
  uint64_t cp = *cpOpt;

  auto ao = arrayOffsets(st);
  if (ao.length < 0 || ao.data < 0) {
    if (err) *err = "Array<T> offsets missing";
    return false;
  }
  auto flen = p.read<uint32_t>((uintptr_t)(*fiArr + ao.length));
  if (!flen || *flen == 0 || *flen > (1u << 22)) {
    if (err) *err = "bad fieldinfo stream length";
    return false;
  }
  auto buf = p.readBytes(*fiArr + ao.data, *flen);
  if (buf.empty()) {
    if (err) *err = "fieldinfo stream unreadable";
    return false;
  }

  U5Reader r{buf.data(), buf.size(), 0};
  uint32_t j = r.next(); // java fields
  uint32_t k = r.next(); // injected fields
  if (j > 4096 || k > 4096) {
    if (err) *err = "implausible field count";
    return false;
  }

  for (uint32_t n = 0; n < j + k; ++n) {
    if (r.hasEnd()) break;
    uint32_t name = r.next();
    uint32_t sig = r.next();
    uint32_t off = r.next();
    uint32_t access = r.next();
    uint32_t fflags = r.next();
    if (fflags & FF_INITIALIZED) r.next();     // initval index
    if (fflags & FF_GENERIC) r.next();         // generic signature
    if (fflags & FF_CONTENDED) r.next();       // contention group

    FieldDesc fd;
    fd.accessFlags = access;
    fd.isStatic = (access & ACC_STATIC) != 0;
    fd.offset = off;
    fd.name = "<name?>";
    fd.descriptor = "<sig?>";

    // CP: u64 entries at cp+80, 1-based; Utf8 entries hold Symbol*.
    for (int which = 0; which < 2; ++which) {
      uint32_t idx = which ? sig : name;
      auto e = p.read<uint64_t>((uintptr_t)(cp + 80 + (uint64_t)(idx - 1) * 8));
      if (!e || *e <= 0x10000) continue;
      auto len = p.read<uint16_t>((uintptr_t)(*e + env.offSymbolLength));
      if (!len || *len == 0 || *len > 512) continue;
      auto body = p.readBytes(*e + env.offSymbolBody, *len);
      if (body.empty()) continue;
      std::string text((const char*)body.data(), body.size());
      (which ? fd.descriptor : fd.name) = std::move(text);
    }
    out.push_back(std::move(fd));
  }
  return !out.empty();
}

std::optional<uint64_t> KlassReader::superClass(const mem::Process& p, const JvmRuntime& rt,
                                                uint64_t klass) {
  auto supOff = rt.structs().field("Klass", "_super");
  if (!supOff) return std::nullopt;
  auto sup = p.read<uint64_t>((uintptr_t)(klass + *supOff));
  if (!sup) return std::nullopt;
  return *sup;
}

void KlassReader::dumpClass(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                            const std::string& label) {
  auto& st = rt.structs();
  const JvmEnv& env = rt.env();

  std::string kname;
  if (env.offKlassName >= 0) {
    auto sym = p.read<uint64_t>((uintptr_t)(klass + env.offKlassName));
    if (sym && *sym) {
      if (auto n = rt.readSymbol(p, *sym)) kname = *n;
    }
  }
  spdlog::debug("fields: [{}] klass={} name='{}'", label, util::hex(klass), kname);

  std::vector<FieldDesc> fields;
  std::string err;
  if (readFields(p, rt, klass, fields, &err)) {
    spdlog::debug("fields: [{}] decoded {} fields:", label, fields.size());
    for (auto& f : fields)
      spdlog::debug("fields:   {} {} {} @ {} [acc=0x{:X}]", f.isStatic ? "static" : "      ",
                    f.descriptor, f.name, f.offset, f.accessFlags);
  } else {
    spdlog::debug("fields: [{}] decode failed: {}", label, err);
  }
}

} // namespace jvm
