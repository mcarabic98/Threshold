#include "jvm/klass_reader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <format>

#include "core/util.h"

namespace jvm {

namespace {

struct ArrayOffsets {
  int64_t length = -1;
  int64_t data = -1;
};

// HotSpot Array<T> layout: {_length int, _data T[]}. _data sits right after
// _length aligned to the element size — u1/u2 at +4, 4-byte elements at +8 —
// so prefer an instantiation with the same element size; fall back to any.
ArrayOffsets arrayOffsets(const VmStructsTable& st, const char* preferred = "Array<int>") {
  ArrayOffsets o;
  const char* order[] = {preferred, "Array<int>", "Array<u2>", "Array<u1>",
                         "Array<u4>", "Array<OopHandle>", "Array<FieldInfo>",
                         "Array<Method*>"};
  for (const char* tn : order) {
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

// Resolve one CP entry as Utf8 text (shared by both field parsers).
std::optional<std::string> cpUtf8(const mem::Process& p, const JvmEnv& env, uint64_t cp,
                                  uint32_t idx) {
  if (idx == 0) return std::nullopt;
  auto e = p.read<uint64_t>((uintptr_t)(cp + 80 + (uint64_t)(idx - 1) * 8));
  if (!e || *e <= 0x10000) return std::nullopt;
  auto len = p.read<uint16_t>((uintptr_t)(*e + env.offSymbolLength));
  if (!len || *len == 0 || *len > 512) return std::nullopt;
  auto body = p.readBytes(*e + env.offSymbolBody, *len);
  if (body.empty()) return std::nullopt;
  return std::string((const char*)body.data(), body.size());
}

bool plausibleFieldText(const std::string& t) {
  if (t.empty() || t.size() > 64) return false;
  for (char c : t)
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return false;
  return true;
}

bool KlassReader::readFields(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                             std::vector<FieldDesc>& out, std::string* err) {
  out.clear();
  auto& st = rt.structs();
  const JvmEnv& env = rt.env();

  auto cOff = st.field("InstanceKlass", "_constants");
  auto cpOpt = cOff ? p.read<uint64_t>((uintptr_t)(klass + *cOff)) : std::nullopt;

  auto fiOff = st.field("InstanceKlass", "_fieldinfo_stream");
  if (!fiOff) {
    // JDK <= 20: fields live in InstanceKlass::_fields — an Array of
    // fixed-size FieldInfo entries (the varint stream appeared later). The
    // entry layout is not self-described by VMStructs; detect it from the
    // data itself (name/signature columns must resolve to CP Utf8).
    auto fOff = st.field("InstanceKlass", "_fields");
    if (!fOff || !cpOpt || !*cpOpt) {
      if (err) *err = "VMStructs missing _fieldinfo_stream/_fields/_constants";
      return false;
    }
    auto fArr = p.read<uint64_t>((uintptr_t)(klass + *fOff));
    if (!fArr || !*fArr) {
      if (err) *err = "unreadable _fields array pointer";
      return false;
    }
    return readFieldsLegacy(p, rt, klass, *fArr, *cpOpt, out, err);
  }
  if (!cOff || !cpOpt || !*cpOpt) {
    if (err) *err = "VMStructs missing _fieldinfo_stream/_constants";
    return false;
  }
  auto fiArr = p.read<uint64_t>((uintptr_t)(klass + *fiOff));
  if (!fiArr || !*fiArr) {
    if (err) *err = "unreadable _fieldinfo_stream";
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

    if (auto t = cpUtf8(p, env, cp, name)) fd.name = std::move(*t);
    if (auto t = cpUtf8(p, env, cp, sig)) fd.descriptor = std::move(*t);
    out.push_back(std::move(fd));
  }
  return !out.empty();
}

// JDK <= 20: _fields is an Array of fixed-size FieldInfo entries. The layout
// is self-described by VMStructs int constants on JDK 17+ (field_slots,
// name/signature/initval index offsets, low/high packed offsets — packed
// carries the field offset). For older JDKs without those constants the
// stride and the name/signature columns are detected from the data itself:
// in every entry those two columns must resolve to CP Utf8 text.
bool KlassReader::readFieldsLegacy(const mem::Process& p, const JvmRuntime& rt, uint64_t klass,
                                   uint64_t fieldsArr, uint64_t cp,
                                   std::vector<FieldDesc>& out, std::string* err) {
  auto& st = rt.structs();
  const JvmEnv& env = rt.env();

  auto ao = arrayOffsets(st, "Array<FieldInfo>");
  if (ao.length < 0 || ao.data < 0) {
    if (err) *err = "Array<T> offsets missing";
    return false;
  }
  auto alen = p.read<uint32_t>((uintptr_t)(fieldsArr + ao.length));
  if (!alen || *alen == 0 || *alen > (1u << 22)) {
    if (err)
      *err = std::format("bad _fields length: arr={} len@{}={} data@{}={}", util::hex(fieldsArr),
                         ao.length, alen ? (long long)*alen : -1ll, ao.data,
                         [&] {
                           auto v = p.read<uint64_t>((uintptr_t)(fieldsArr + ao.data));
                           return v ? util::hex(*v) : std::string("?");
                         }());
    return false;
  }
  auto buf = p.readBytes(fieldsArr + ao.data, (size_t)*alen * 2);
  if (buf.empty()) {
    if (err) *err = "_fields unreadable";
    return false;
  }
  const uint16_t* sh = reinterpret_cast<const uint16_t*>(buf.data());
  size_t total = buf.size() / 2;

  // One-shot raw dump: VMStructs FieldInfo descriptions (if any) + head bytes.
  static bool dumped = false;
  if (!dumped) {
    dumped = true;
    rt.dumpMatches("FieldInfo");
    spdlog::debug("fields: legacy _fields @ {} : {} u2s, head: {}", util::hex(fieldsArr),
                  total, hexDump(buf.data(), std::min<size_t>(buf.size(), 128)));
  }

  // Preferred: the VMStructs int constants describe the FieldInfo entry on
  // JDK 17+ ("FieldInfo::field_slots", "::name_index_offset", ...). Access
  // flags sit in short[0] (the one slot VMStructs does not name); the field
  // offset is packed into low|high (24 bits are plenty for instance sizes).
  auto ic = [&](const char* name) -> std::optional<int64_t> {
    return st.intConstant(std::string("FieldInfo::") + name);
  };
  int bestStride = 0, nameCol = -1, sigCol = -1, accCol = -1, offCol = -1;
  double bestScore = 0;
  if (ic("field_slots") && ic("name_index_offset") && ic("signature_index_offset") &&
      ic("low_packed_offset")) {
    bestStride = (int)*ic("field_slots");
    nameCol = (int)*ic("name_index_offset");
    sigCol = (int)*ic("signature_index_offset");
    accCol = 0;
    offCol = (int)*ic("low_packed_offset");
    bestScore = 1.0;
  } else {
    // Auto-detection fallback (older JDKs): name and signature columns must
    // resolve to CP Utf8 in every entry.
    size_t probe = std::min<size_t>(total, 120);
    auto colScore = [&](int stride, int col, size_t entries) {
      double s = 0;
      for (size_t e = 0; e < entries; ++e) {
        auto t = cpUtf8(p, env, cp, sh[e * (size_t)stride + (size_t)col]);
        s += t && plausibleFieldText(*t) ? 1 : 0;
      }
      return s / (double)entries;
    };
    for (int stride : {5, 6, 7, 8}) {
      size_t entries = probe / stride;
      if (entries < 6) continue;
      for (int c1 = 0; c1 < stride; ++c1)
        for (int c2 = 0; c2 < stride; ++c2) {
          if (c1 == c2) continue;
          double s = std::min(colScore(stride, c1, entries), colScore(stride, c2, entries));
          if (s > bestScore) {
            bestScore = s;
            bestStride = stride;
            nameCol = c1;
            sigCol = c2;
          }
        }
    }
    if (bestStride == 0 || bestScore < 0.9) {
      if (err) *err = std::format("legacy layout undetected (stride={} score={:.2f})",
                                  bestStride, bestScore);
      return false;
    }

    // Name vs signature: check several entries — descriptors start with
    // 'L'/'[' or a primitive letter; names never contain '/' or ';'.
    auto looksLikeDescriptor = [](const std::string& t) {
      return !t.empty() && (t[0] == 'L' || t[0] == '[' ||
                            (t[0] >= 'B' && t[0] <= 'D' && t.size() == 1) || t[0] == 'F' ||
                            (t[0] == 'I' && t.size() == 1) || (t[0] == 'J' && t.size() == 1) ||
                            (t[0] == 'S' && t.size() == 1) || (t[0] == 'Z' && t.size() == 1));
    };
    int descVotes = 0, nameVotes = 0;
    for (size_t e = 0; e < std::min<size_t>(probe / (size_t)bestStride, 8); ++e) {
      if (auto t = cpUtf8(p, env, cp, sh[e * (size_t)bestStride + (size_t)sigCol]))
        descVotes += looksLikeDescriptor(*t) ? 1 : 0;
      if (auto t = cpUtf8(p, env, cp, sh[e * (size_t)bestStride + (size_t)nameCol]))
        nameVotes += looksLikeDescriptor(*t) ? 0 : 1;
    }
    if (descVotes == 0 && nameVotes > 0) std::swap(nameCol, sigCol);
  }
  // access/offset columns for the fallback path (VMStructs path set them
  // above); offset column is monotonic across entries, access is short flags.
  if (accCol < 0 || offCol < 0) {
    size_t entries = total / (size_t)bestStride;
    for (int c1 = 0; c1 < bestStride; ++c1) {
      if (c1 == nameCol || c1 == sigCol) continue;
      for (int c2 = 0; c2 < bestStride; ++c2) {
        if (c2 == nameCol || c2 == sigCol || c2 == c1) continue;
        size_t ok = 0, mono = 0;
        uint32_t prev = 0;
        for (size_t e = 0; e < entries; ++e) {
          uint32_t v = sh[e * (size_t)bestStride + (size_t)c2];
          if (v > 0 && v <= (1u << 16) - 4) ++ok;
          if (v >= prev) ++mono;
          prev = v;
        }
        if (ok == entries && (double)mono / entries > 0.85) {
          accCol = c1;
          offCol = c2;
          break;
        }
      }
      if (offCol >= 0) break;
    }
    if (offCol < 0) {
      // positional convention: the first non-utf8 column is access, next is
      // the offset
      for (int c1 = 0; c1 < bestStride; ++c1) {
        if (c1 == nameCol || c1 == sigCol) continue;
        bool allZero = true;
        for (size_t e = 0; e < entries; ++e)
          if (sh[e * (size_t)bestStride + (size_t)c1] != 0) { allZero = false; break; }
        if (!allZero) { accCol = c1; break; }
      }
      if (accCol >= 0 && accCol + 1 < bestStride) offCol = accCol + 1;
    }
  }
  size_t entries = total / (size_t)bestStride;
  spdlog::debug("fields: legacy layout: stride={} nameCol={} sigCol={} accCol={} offCol={} "
                "(score {:.2f}, {} entries)",
                bestStride, nameCol, sigCol, accCol, offCol, bestScore, entries);
  if (accCol < 0 || offCol < 0) {
    if (err) *err = "legacy access/offset columns unresolved";
    return false;
  }

  for (size_t e = 0; e < entries; ++e) {
    const uint16_t* row = sh + e * (size_t)bestStride;
    FieldDesc fd;
    fd.accessFlags = row[accCol];
    fd.isStatic = (fd.accessFlags & ACC_STATIC) != 0;
    // JDK 17 packs the field offset into low|high with 2 tag bits at the
    // bottom: real_offset = (low | high<<16) >> 2. Verified live against
    // java/lang/String (value/coder/hash) and the Minecraft class (static
    // mirror base 112 matches the JDK 21 session).
    uint32_t packed = (uint32_t)row[offCol];
    if (int hi = offCol + 1; hi < bestStride && hi != nameCol && hi != sigCol && hi != accCol)
      packed |= (uint32_t)row[hi] << 16;
    fd.offset = packed >> 2;
    fd.name = "<name?>";
    fd.descriptor = "<sig?>";
    if (auto t = cpUtf8(p, env, cp, row[nameCol])) fd.name = std::move(*t);
    if (auto t = cpUtf8(p, env, cp, row[sigCol])) fd.descriptor = std::move(*t);
    if (plausibleFieldText(fd.name) && plausibleFieldText(fd.descriptor) &&
        fd.name != "<name?>" && fd.descriptor != "<sig?>")
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
