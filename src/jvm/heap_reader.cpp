#include "jvm/heap_reader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <unordered_set>

#include "core/util.h"
#include "jvm/klass_reader.h"

namespace jvm {

namespace {

constexpr size_t kMaxRegionSpan = 64u << 20;  // sanity cap per contiguous read
constexpr size_t kFallbackChunk = 8u << 20;   // linear-scan chunk
constexpr uint32_t kMaxArrayLen = 1u << 27;
constexpr size_t kSampleRegions = 16;
constexpr size_t kCalibRegions = 8;
constexpr size_t kTopIds = 40;
constexpr size_t kMaxAddrsPerTarget = 16;

inline uint64_t loadU(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  std::memcpy(&v, p, n);
  return v;
}

inline size_t align8(size_t v) { return (v + 7) & ~(size_t)7; }

} // namespace

bool HeapReader::resolve(const mem::Process& p, JvmRuntime& rt) {
  ready_ = false;
  g1Ready_ = false;
  fallbackReady_ = false;
  calibratedMode_ = 0;
  collectedHeap_ = 0;
  rlDumped_ = false;
  rlHistDumped_ = false;

  auto& st = rt.structs();
  auto ch = st.staticDatum("Universe", "_collectedHeap");
  if (!ch) {
    spdlog::warn("heap: Universe::_collectedHeap missing");
    return false;
  }
  auto ptr = p.read<uint64_t>((uintptr_t)*ch);
  if (!ptr || !*ptr) {
    spdlog::warn("heap: collected heap pointer unreadable @ {}", util::hex(*ch));
    return false;
  }
  collectedHeap_ = *ptr;
  spdlog::info("heap: CollectedHeap @ {}", util::hex(collectedHeap_));

  // G1 region walk structures.
  offG1Hrm_ = st.field("G1CollectedHeap", "_hrm").value_or(-1);
  offHrmRegions_ = st.field("HeapRegionManager", "_regions").value_or(-1);
  // JDK 21 splits the GrowableArray description: "_data" lives on one
  // instantiation, "_len" on the GrowableArrayBase base struct.
  for (const char* tn : {"GrowableArray<int>", "GrowableArray<HeapRegion*>",
                         "GrowableArray<HeapWord*>", "GrowableArray<MetaWord*>",
                         "GrowableArray<JavaThread*>"}) {
    auto d = st.field(tn, "_data");
    if (d) {
      offGarrData_ = *d;
      break;
    }
  }
  if (auto l = st.field("GrowableArrayBase", "_len"))
    offGarrLen_ = *l;
  else
    offGarrLen_ = st.field("GrowableArray<int>", "_len").value_or(-1);
  auto bot = st.field("HeapRegion", "_bottom");
  auto top = st.field("HeapRegion", "_top");
  if (!bot) bot = st.field("ContiguousSpace", "_bottom");
  if (!top) top = st.field("ContiguousSpace", "_top");
  offRegionBottom_ = bot.value_or(-1);
  offRegionTop_ = top.value_or(-1);

  g1Ready_ = offG1Hrm_ >= 0 && offHrmRegions_ >= 0 && offGarrData_ >= 0 && offGarrLen_ >= 0 &&
             offRegionBottom_ >= 0 && offRegionTop_ >= 0;
  spdlog::info(
      "heap: G1 walk {} (hrm={} hrmRegions={} garr={}/{} region={}/{})",
      g1Ready_ ? "ready" : "missing", offG1Hrm_, offHrmRegions_, offGarrData_, offGarrLen_,
      offRegionBottom_, offRegionTop_);

  // Fallback: CollectedHeap::_reserved as [start, end). JDK 21 dropped the
  // MemRegion VMStructs entries, so the raw pair is parsed directly — the
  // range is also what scopes the oop-decode hypotheses downstream.
  if (auto rr = reservedRange(p, rt)) {
    heapStart_ = rr->first;
    heapEnd_ = rr->second;
    fallbackReady_ = true;
  }
  spdlog::info("heap: fallback linear scan {} [{} .. {}]",
               fallbackReady_ ? "ready" : "missing", util::hex(heapStart_),
               util::hex(heapEnd_));

  ready_ = g1Ready_ || fallbackReady_;

  // JDK 21+ dropped the narrow-klass statics from VMStructs: derive the
  // klass-pointer decode empirically.
  if (ready_ && !rt.env().compressedKlasses) deriveKlassDecode(p, rt);

  return ready_;
}

HeapReader::RegionList HeapReader::collectRegions(const mem::Process& p,
                                                  const JvmRuntime& rt) const {
  RegionList rl;
  if (!g1Ready_) {
    rl.error = "no G1 walk";
    return rl;
  }
  // JDK 21 update builds embed HeapRegionManager inside G1CollectedHeap
  // (_hrm holds the object itself; the qword there is its first member, the
  // _bot_mapper pointer); JDK <= 20 and early 21 store a HeapRegionManager*
  // in the same field. Try both layouts and let the table contents decide —
  // the wrong structure yields no usable region set.
  std::vector<uint64_t> hrms{collectedHeap_ + (uint64_t)offG1Hrm_};
  if (auto ptr = p.read<uint64_t>((uintptr_t)(collectedHeap_ + (uint64_t)offG1Hrm_)))
    if (*ptr > 0x10000 && *ptr < 0x7FFFFFFFFFFFull && *ptr != hrms.front())
      hrms.push_back(*ptr);

  RegionList best;
  for (size_t i = 0; i < hrms.size(); ++i) {
    RegionList rl = collectRegionsHrm(hrms[i], p, rt);
    if (rl.ok && rl.size() >= 2) {
      if (hrms.size() > 1)
        spdlog::debug("heap: region layout candidate #{} (hrm {}) accepted, {} regions", i,
                      util::hex(hrms[i]), rl.size());
      return rl;
    }
    if (!best.ok || rl.size() > best.size()) best = std::move(rl);
  }
  if (!best.ok && best.error.empty()) best.error = "no usable region table";
  return best;
}

HeapReader::RegionList HeapReader::collectRegionsHrm(uint64_t hrm, const mem::Process& p,
                                                     const JvmRuntime& rt) const {
  RegionList rl;
  uint64_t regionsBase = hrm + (uint64_t)offHrmRegions_;

  // JDK <= 20: _regions is a GrowableArray (len @GrowableArrayBase._len,
  // data @GrowableArray<T>::_data). JDK 21+: it is a G1HeapRegionTable
  // (_base @16 -> HeapRegion*[] , _length @24). Prefer the table when its
  // fields are described.
  uint64_t dataP = 0;
  int64_t len = -1;
  auto& st = rt.structs();
  auto tabBase = st.field("G1HeapRegionTable", "_base");
  auto tabLen = st.field("G1HeapRegionTable", "_length");
  if (tabBase && tabLen) {
    auto d = p.read<uint64_t>((uintptr_t)(regionsBase + *tabBase));
    auto l = p.read<uint64_t>((uintptr_t)(regionsBase + *tabLen));
    if (d && l) {
      dataP = *d;
      len = (int64_t)*l;
    }
  } else {
    auto d = p.read<uint64_t>((uintptr_t)(regionsBase + offGarrData_));
    auto l = p.read<int32_t>((uintptr_t)(regionsBase + offGarrLen_));
    if (d && l) {
      dataP = *d;
      len = *l;
    }
  }

  if (!rlDumped_) {
    rlDumped_ = true;
    spdlog::debug("heap: hrm={} regionsBase={} data={} len={}", util::hex(hrm),
                  util::hex(regionsBase), util::hex(dataP), len);
  }
  if (!dataP || len <= 0 || len > (1 << 22)) {
    rl.error = std::format("region array unreadable (data={} len={})", util::hex(dataP), len);
    return rl;
  }
  auto arr = p.readBytes(dataP, (size_t)len * 8);
  if (arr.empty()) {
    rl.error = "region array buffer unreadable";
    return rl;
  }
  rl.ok = true;
  // Java heap range — used to reject bogus spans that fall outside the heap.
  uint64_t heapLo = 0, heapHi = 0;
  if (auto rr = reservedRange(p, rt)) {
    heapLo = rr->first;
    heapHi = rr->second;
    if (!rlHistDumped_)
      spdlog::debug("heap: reserved range [{} .. {}]", util::hex(heapLo), util::hex(heapHi));
  }
  auto tagOff = st.field("HeapRegion", "_type");
  std::map<uint32_t, int> tagHist;
  for (int64_t i = 0; i < len; ++i) {
    uint64_t reg;
    std::memcpy(&reg, arr.data() + (size_t)i * 8, 8);
    if (!reg) continue;
    auto b = p.read<uint64_t>((uintptr_t)(reg + offRegionBottom_));
    auto t = p.read<uint64_t>((uintptr_t)(reg + offRegionTop_));
    if (!b || !t || !*b || *t <= *b || *t - *b > kMaxRegionSpan) continue;
    if (heapLo && (*b < heapLo || *t > heapHi)) continue;
    uint32_t tag = 0xFFFFFFFF;
    if (tagOff && *tagOff >= 0) {
      auto tagV = p.read<uint32_t>((uintptr_t)(reg + *tagOff));
      if (tagV) tag = *tagV;
    }
    ++tagHist[tag];
    rl.bottoms.push_back(*b);
    rl.tops.push_back(*t);
    rl.tags.push_back(tag);
  }
  if (!rlHistDumped_) {
    rlHistDumped_ = true;
    std::string hist;
    for (auto& [tag, n] : tagHist) hist += std::format("[{:X}:{}] ", tag, n);
    spdlog::debug("heap: {} usable regions, tag histogram: {}", rl.size(), hist);
  }
  return rl;
}

std::optional<std::pair<uint64_t, uint64_t>> HeapReader::reservedRange(
    const mem::Process& p, const JvmRuntime& rt) const {
  if (!collectedHeap_) return std::nullopt;
  auto res = rt.structs().field("CollectedHeap", "_reserved");
  if (!res) return std::nullopt;
  // JDK 21 VMStructs publishes MemRegion::_word_size — the size in HeapWords
  // (8 bytes each on x64), not bytes; JDK <= 20 exposed no MemRegion fields at
  // all. Decide by the published name, fall back to shape heuristics.
  auto v0 = p.read<uint64_t>((uintptr_t)(collectedHeap_ + *res));
  auto v1 = p.read<uint64_t>((uintptr_t)(collectedHeap_ + *res + 8));
  if (!v0 || !v1 || !*v0) return std::nullopt;
  uint64_t lo = 0, hi = 0;
  if (rt.structs().field("MemRegion", "_word_size").has_value()) {
    lo = *v0;
    hi = lo + *v1 * 8; // HeapWord = 8 bytes (x64-only targets)
  } else if (*v1 > (1u << 20) && *v1 < (1ull << 36) && *v1 % (2u << 20) == 0) {
    lo = *v0;
    hi = *v0 + *v1;
  } else if (*v1 > *v0) {
    lo = *v0;
    hi = *v1;
  }
  if (!lo || hi <= lo || hi - lo > (1ull << 36)) return std::nullopt;
  return std::make_pair(lo, hi);
}

std::map<uint64_t, size_t> HeapReader::sampleKlassIds(const mem::Process& p,
                                                      const RegionList& rl,
                                                      int maxRegions) const {
  // Sample eden/survivor/old regions first — archive regions (start of the
  // heap, ZIP/CDS data) poison the id statistics. Only positions that look
  // like object starts contribute: a plain unlocked mark word is 0x...001.
  std::map<uint64_t, size_t> freq;
  int used = 0;
  auto plausible = [](uint32_t tag) {
    return tag == 2 || tag == 3 || tag == 8; // Eden / Survivor / Old
  };
  for (int pass = 0; pass < 2 && used < maxRegions; ++pass) {
    for (size_t r = 0; r < rl.size() && used < maxRegions; ++r) {
      bool ok = plausible(rl.tags[r]);
      if ((pass == 0) != ok) continue;
      auto buf = p.readBytes(rl.bottoms[r], (size_t)(rl.tops[r] - rl.bottoms[r]));
      if (buf.empty()) continue;
      ++used;
      size_t pos = 0;
      while (pos + 16 <= buf.size()) {
        uint64_t mark = loadU(buf.data() + pos, 8);
        if ((mark & 0x7) == 1 && mark != 0) {
          uint32_t id;
          std::memcpy(&id, buf.data() + pos + 8, 4);
          if (id) ++freq[id];
        }
        pos += 8;
      }
    }
  }
  return freq;
}

bool HeapReader::deriveKlassDecode(const mem::Process& p, JvmRuntime& rt) {
  auto& st = rt.structs();

  // 1. vmClasses::_klasses[] -> array of InstanceKlass* for fundamental classes.
  uint64_t arrAddr = 0;
  if (auto it = st.statics.find("vmClasses"); it != st.statics.end())
    for (auto& [f, v] : it->second)
      if (f.rfind("_klasses[", 0) == 0) {
        arrAddr = v;
        break;
      }

  KlassCache cache;
  std::vector<std::pair<std::string, uint64_t>> known;
  if (arrAddr) {
    for (int i = 0; i < 200; ++i) {
      auto k = p.read<uint64_t>((uintptr_t)(arrAddr + (uint64_t)i * 8));
      if (!k || !*k || *k < 0x10000) continue; // skip holes, keep scanning
      const KlassInfo& ki = klassInfo(p, rt, *k, cache);
      if (!ki.valid || ki.name.empty()) continue;
      known.emplace_back(ki.name, *k);
    }
    spdlog::debug("heap: vmClasses[{}] -> {} named klasses", util::hex(arrAddr), known.size());
  }

  // 2. Sample narrow ids from the heap.
  RegionList rl = collectRegions(p, rt);
  if (rl.size() == 0) {
    spdlog::warn("heap: klass decode derivation skipped ({})", rl.error);
    return false;
  }
  auto ids = sampleKlassIds(p, rl, (int)kSampleRegions);
  // Rank by frequency (the map is id-ordered; raw id values are meaningless).
  std::vector<std::pair<uint64_t, size_t>> ranked(ids.begin(), ids.end());
  std::sort(ranked.begin(), ranked.end(),
            [](auto& a, auto& b) { return a.second > b.second; });
  std::vector<uint64_t> top;
  for (auto& [id, n] : ranked)
    if ((int)top.size() < (int)kTopIds && n >= 2) top.push_back(id);
  // Candidate id pool for anchor pairing: every frequent id (the id of the
  // anchor class itself must be in there, we just don't know which one).
  std::vector<uint64_t> pool;
  for (auto& [id, n] : ranked)
    if (n >= 10 && (int)pool.size() < 300) pool.push_back(id);
  spdlog::debug("heap: sampled {} distinct ids, top {}, pool {}", ids.size(), top.size(),
                pool.size());
  if (top.size() < 5) return false;

  // 3a. Preferred hypothesis (JDK 17+ on Windows with CDS): the compressed
  // klass base equals the shared-metaspace base (archive maps at a fixed
  // low address; runtime class space is placed within 4 GB of it).
  struct Cand {
    uint64_t base;
    int shift;
    int valid;
  };
  std::vector<Cand> cands;
  std::vector<uint64_t> probe; // frequent (real) ids for validation
  {
    for (auto& [id, n] : ranked) {
      if (n < 50) break;
      probe.push_back(id);
      if ((int)probe.size() >= 2000) break;
    }
    for (uint64_t t : top) probe.push_back(t);
  }
  auto validate = [&](uint64_t base, int shift) -> int {
    int valid = 0;
    for (uint64_t id : probe) {
      if (klassInfo(p, rt, base + (id << shift), cache).valid) ++valid;
    }
    return valid;
  };
  // Hypothesis bases: MetaspaceObj::_shared_metaspace_base when nonzero, plus
  // the fixed Windows CDS mapping (classes map at 0x8_00000000, ids are the
  // raw offsets). The vmClass cross-check below is the real gate.
  std::vector<uint64_t> hypoBases;
  if (auto smb = st.staticDatum("MetaspaceObj", "_shared_metaspace_base"))
    if (auto v = p.read<uint64_t>((uintptr_t)*smb); v && *v) hypoBases.push_back(*v);
  hypoBases.push_back(0x000800000000ull);
  for (uint64_t hb : hypoBases) {
    for (int shift : {0, 3}) {
      int valid = validate(hb, shift);
      spdlog::debug("heap: hypothesis base={} shift={} -> {}/{} ids decode valid",
                    util::hex(hb), shift, valid, probe.size());
      if (valid >= (int)probe.size() / 4) cands.push_back({hb, shift, valid});
    }
  }

  // 3b. Solve for (base, shift) by pairing known vmClasses addresses with
  // frequent ids. Most vmClasses are abstract (no heap instances), so anchor
  // on every known address; a cheap 10-id probe filters before full scoring.
  if (!known.empty() && cands.empty()) {
    for (int shift : {0, 3}) {
      for (auto& [kname, kaddr] : known) {
        for (uint64_t id : pool) {
          uint64_t base = kaddr - (id << shift);
          if (!base) continue;
          int quick = 0;
          for (int q = 0; q < 10; ++q)
            if (klassInfo(p, rt, base + (top[q % top.size()] << shift), cache).valid)
              ++quick;
          if (quick < 9) continue;
          int valid = validate(base, shift);
          if (valid >= (int)probe.size() * 9 / 10)
            cands.push_back({base, shift, valid});
        }
      }
    }
  }

  if (!cands.empty()) {
    std::sort(cands.begin(), cands.end(),
              [](auto& a, auto& b) { return a.valid > b.valid; });
    // Cross-check: for each vmClasses entry WITH heap instances, the id that
    // decodes to that exact Klass address must appear in the sample.
    auto& c = cands.front();
    int matches = 0, checked = 0;
    for (auto& [kname, kaddr] : known) {
      if (kaddr < c.base) continue;
      uint64_t diff = kaddr - c.base;
      if (c.shift == 3 && (diff & 7)) continue;
      uint64_t id = c.shift == 3 ? diff >> 3 : diff;
      ++checked;
      if (ids.count(id)) ++matches;
    }
    if (matches >= 3) {
      rt.setNarrowKlass(c.base, c.shift);
      spdlog::info("heap: derived narrow klass base={} shift={} (valid {}/{}, vmClass "
                   "matches {}/{})",
                   util::hex(c.base), c.shift, c.valid, probe.size(), matches, checked);
      return true;
    }
    spdlog::debug("heap: best candidate rejected (matches={}/{})", matches, checked);
  }

  // 4. Fallback: wide (uncompressed) class pointers — sample u64 values at
  // object starts and validate them directly as Klass* addresses.
  std::vector<uint64_t> wideSamples;
  for (size_t r = 0; r < rl.size(); ++r) {
    if ((int)wideSamples.size() >= (int)kTopIds) break;
    auto buf = p.readBytes(rl.bottoms[r], (size_t)(rl.tops[r] - rl.bottoms[r]));
    if (buf.empty()) continue;
    for (size_t pos = 0; pos + 16 <= buf.size() && (int)wideSamples.size() < (int)kTopIds;
         pos += 8) {
      uint64_t mark = loadU(buf.data() + pos, 8);
      if ((mark & 0x7) != 1 || mark == 0) continue;
      uint64_t ka = loadU(buf.data() + pos + 8, 8);
      if (ka > 0x10000) wideSamples.push_back(ka);
    }
  }
  int wideValid = 0, wideNamed = 0;
  for (uint64_t t : wideSamples) {
    const KlassInfo& ki = klassInfo(p, rt, t, cache);
    if (ki.valid) {
      ++wideValid;
      if (!ki.name.empty()) ++wideNamed;
    }
  }
  if (wideValid >= (int)wideSamples.size() * 9 / 10 && wideValid >= 5) {
    rt.setWideKlasses();
    spdlog::info("heap: wide klass pointers detected (valid {}/{})", wideValid,
                 wideSamples.size());
    return true;
  }
  spdlog::warn("heap: klass decode derivation failed (compressed candidates={}, wide {}/{})",
               cands.size(), wideValid, wideSamples.size());
  return false;
}

// Hot klass cache: key = decoded Klass* address. Per-thread caches are
// checked first (lock-free); misses fall through to the shared table (mutex,
// RPM resolution happens once per klass process-wide).
const HeapReader::KlassInfo& HeapReader::resolve(uint64_t key, WalkCtx& ctx, ScanState& st,
                                                 const mem::Process& p, const JvmRuntime& rt,
                                                 const std::vector<std::string>& targets) const {
  auto it = ctx.local.find(key);
  if (it != ctx.local.end()) return it->second;
  {
    std::lock_guard g(st.mtx);
    auto git = st.table.find(key);
    if (git != st.table.end()) {
      auto [i, ok] = ctx.local.emplace(key, git->second);
      return i->second;
    }
  }
  KlassInfo ki = klassInfo(p, rt, key, ctx.local); // RPM resolution; may already
                                                   // have created the local entry
  for (size_t i = 0; i < targets.size(); ++i)
    if (ki.name == targets[i]) {
      ki.targetIdx = (int)i;
      break;
    }
  std::lock_guard g(st.mtx);
  auto [ins, inserted] = st.table.emplace(key, std::move(ki));
  // klassInfo() may have pre-created the local entry without targetIdx —
  // refresh it from the global copy or it shadows the correct entry.
  auto lit = ctx.local.find(key);
  if (lit == ctx.local.end()) ctx.local.emplace(key, ins->second);
  else lit->second = ins->second;
  return ctx.local.find(key)->second;
}

// Symbol text in one RPM when possible: Symbol::_length is immediately
// followed by _body, so a single 72-byte read covers names up to ~70 chars.
const HeapReader::KlassInfo& HeapReader::klassInfo(const mem::Process& p, const JvmRuntime& rt,
                                                   uint64_t klassAddr,
                                                   KlassCache& cache) const {
  static const KlassInfo invalid{};
  auto it = cache.find(klassAddr);
  if (it != cache.end()) return it->second;

  KlassInfo ki;
  const JvmEnv& env = rt.env();
  if (klassAddr) {
    auto lh = p.read<uint32_t>((uintptr_t)(klassAddr + env.offKlassLayoutHelper));
    if (lh) {
      uint32_t v = *lh;
      bool plausible;
      if (env.lhIsArray(v)) {
        size_t es = env.arrayElemSize(v);
        plausible = es >= 1 && es <= 8;
      } else {
        // Mode-independent plausibility: the calibrated instance-size mode is
        // chosen later; any sane instance lh fits [16, 64 MB] in some mode.
        plausible = v >= 16 && v <= (1u << 26);
      }
      if (plausible) {
        ki.lh = v;
        ki.valid = true;
        if (env.offKlassName >= 0) {
          auto sym = p.read<uint64_t>((uintptr_t)(klassAddr + env.offKlassName));
          if (sym && *sym) {
            if (auto name = rt.readSymbol(p, *sym)) {
              ki.name = std::move(*name);
              ki.mc = ki.name.rfind("net/minecraft/", 0) == 0;
            }
          }
        }
      }
    }
  }
  auto [i, ok] = cache.emplace(klassAddr, std::move(ki));
  return i->second;
}

void HeapReader::walkRegion(uint64_t bottom, uint64_t top, WalkCtx& ctx, ScanState& st,
                            const mem::Process& p, const JvmRuntime& rt,
                            const std::vector<std::string>& targets, int sizeMode) const {
  const JvmEnv& env = rt.env();
  if (top <= bottom || top - bottom > kMaxRegionSpan) return;

  auto buf = p.readBytes(bottom, (size_t)(top - bottom));
  if (buf.empty()) return; // uncommitted / unreadable

  const size_t word = 8;
  const size_t minObj = env.headerSize() > 16 ? 24 : 16;
  const size_t lenOff = env.arrayLengthOffset();

  // Probable object start after a chain break: unlocked mark (0x...001) plus
  // a klass id that resolves to a valid Klass. RPM cost is bounded by the
  // klass cache (garbage ids fail fast on the layout-helper read).
  auto findStart = [&](size_t from) -> size_t {
    size_t scan = from;
    while (scan + minObj <= buf.size()) {
      uint64_t mark = loadU(buf.data() + scan, 8);
      if ((mark & 0x7) == 1 && mark != 0) {
        uint64_t kid = env.compressedKlasses ? loadU(buf.data() + scan + 8, 4)
                                             : loadU(buf.data() + scan + 8, 8);
        if (kid) {
          uint64_t key = env.compressedKlasses
                             ? env.narrowKlassBase + (kid << env.narrowKlassShift)
                             : kid;
          const KlassInfo& ki = resolve(key, ctx, st, p, rt, targets);
          if (ki.valid) return scan;
        }
      }
      scan += word;
    }
    return buf.size(); // nothing found till the end
  };

  size_t pos = 0;
  while (pos + minObj <= buf.size()) {
    const uint8_t* o = buf.data() + pos;
    uint64_t klassRaw = env.compressedKlasses ? loadU(o + 8, 4) : loadU(o + 8, 8);
    uint64_t key = 0;
    if (klassRaw != 0)
      key = env.compressedKlasses ? env.narrowKlassBase + (klassRaw << env.narrowKlassShift)
                                  : klassRaw;
    const KlassInfo* ki = nullptr;
    if (key) {
      const KlassInfo& info = resolve(key, ctx, st, p, rt, targets);
      if (info.valid) ki = &info;
    }
    if (!ki) {
      // Chain break: scan forward for the next probable object start.
      size_t next = findStart(pos + word);
      if (next >= buf.size()) break;
      pos = next;
      continue;
    }

    size_t sz;
    if (env.lhIsArray(ki->lh)) {
      uint32_t len = (uint32_t)loadU(o + lenOff, 4);
      if (len > kMaxArrayLen) {
        pos += word;
        continue;
      }
      sz = align8(env.arrayHeaderSize(ki->lh) + (size_t)len * env.arrayElemSize(ki->lh));
    } else {
      sz = env.instanceSize(ki->lh, sizeMode);
    }
    if (sz < minObj || sz > kMaxRegionSpan || (sz & 7)) {
      size_t next = findStart(pos + word);
      if (next >= buf.size()) break;
      pos = next;
      continue;
    }
    if (pos + sz > buf.size()) break; // truncated tail

    if (ki->targetIdx >= 0) {
      ++ctx.counts[ki->targetIdx];
      auto& v = ctx.addrs[ki->targetIdx];
      if (v.size() < kMaxAddrsPerTarget) v.push_back(bottom + pos);
    }
    if (ki->mc) ++ctx.mc;
    ++ctx.objects;
    pos += sz;
  }
  ++ctx.regions;
}

HeapScanResult HeapReader::scan(const mem::Process& p, const JvmRuntime& rt,
                                const std::vector<std::string>& targets,
                                std::atomic<bool>* cancel) {
  HeapScanResult r;
  if (!ready_) {
    r.error = "heap anchors not resolved";
    return r;
  }
  if (!rt.env().valid) {
    r.error = "jvm env incomplete";
    return r;
  }
  auto t0 = std::chrono::steady_clock::now();

  RegionList rl = collectRegions(p, rt);
  if (!rl.ok) {
    r.error = rl.error;
    return r;
  }
  if (cancel && cancel->load()) {
    r.error = "cancelled";
    return r;
  }

  ScanState state;
  state.table.reserve(1 << 15);

  // Calibration: instance-size encoding on a small sample (cheap — the klass
  // table is shared with the main walk and built here).
  {
    const JvmEnv& env = rt.env();
    std::vector<std::pair<uint64_t, uint64_t>> sample;
    size_t taken = 0;
    for (size_t r2 = 0; r2 < rl.size() && taken < kCalibRegions; ++r2) {
      auto plausible = [](uint32_t tag) { return tag == 2 || tag == 3 || tag == 8; };
      if (!plausible(rl.tags[r2])) continue;
      sample.emplace_back(rl.bottoms[r2], rl.tops[r2]);
      ++taken;
    }
    if (sample.empty()) {
      r.error = "no committed regions found";
      return r;
    }
    struct Score {
      size_t named = 0;
      size_t resync = 0;
    };
    std::map<int, Score> scores;
    WalkCtx calibCtx;
    calibCtx.local.reserve(1 << 14);
    const size_t minObj = env.headerSize() > 16 ? 24 : 16;
    for (int mode : {1, 2, 3, 4, 5}) {
      Score sc;
      for (auto [b, t] : sample) {
        if (cancel && cancel->load()) break;
        auto buf = p.readBytes(b, (size_t)(t - b));
        if (buf.empty()) continue;
        size_t pos = 0;
        while (pos + minObj <= buf.size()) {
          const uint8_t* o = buf.data() + pos;
          uint64_t klassRaw = env.compressedKlasses ? loadU(o + 8, 4) : loadU(o + 8, 8);
          if (klassRaw == 0) {
            pos += 8;
            ++sc.resync;
            continue;
          }
          uint64_t key = env.compressedKlasses
                             ? env.narrowKlassBase + (klassRaw << env.narrowKlassShift)
                             : klassRaw;
          const KlassInfo& ki = resolve(key, calibCtx, state, p, rt, targets);
          if (!ki.valid) {
            pos += 8;
            ++sc.resync;
            continue;
          }
          size_t sz;
          if (env.lhIsArray(ki.lh)) {
            uint32_t len = (uint32_t)loadU(o + env.arrayLengthOffset(), 4);
            if (len > kMaxArrayLen) {
              pos += 8;
              ++sc.resync;
              continue;
            }
            sz = align8(env.arrayHeaderSize(ki.lh) + (size_t)len * env.arrayElemSize(ki.lh));
          } else {
            sz = env.instanceSize(ki.lh, mode);
          }
          if (sz < minObj || sz > kMaxRegionSpan || (sz & 7) || pos + sz > buf.size()) {
            pos += 8;
            ++sc.resync;
            continue;
          }
          if (!ki.name.empty()) ++sc.named;
          pos += sz;
        }
      }
      scores[mode] = sc;
      spdlog::debug("heap: calibration mode {} -> named={} resync={}", mode, sc.named,
                    sc.resync);
    }
    int best = 2;
    long long bestScore = -1;
    for (auto& [mode, sc] : scores) {
      long long s = (long long)sc.named * 2 - (long long)sc.resync;
      if (s > bestScore) {
        bestScore = s;
        best = mode;
      }
    }
    calibratedMode_ = best;
    spdlog::info("heap: instance size mode {} calibrated", best);
  }

  // Parallel walk over all committed regions.
  const size_t nThreads = std::clamp<size_t>(
      std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4, 1, 6);
  std::atomic<size_t> nextRegion{0};
  std::vector<WalkCtx> ctxs(nThreads);
  for (auto& c : ctxs) {
    c.local.reserve(1 << 16);
    c.addrs.resize(targets.size());
    c.counts.assign(targets.size(), 0);
  }
  std::vector<std::thread> threads;
  std::exception_ptr wex = nullptr;
  std::mutex exMtx;
  for (size_t t = 0; t < nThreads; ++t) {
    threads.emplace_back([&, t] {
      try {
        for (;;) {
          if (cancel && cancel->load()) break;
          size_t r = nextRegion.fetch_add(1);
          if (r >= rl.size()) break;
          walkRegion(rl.bottoms[r], rl.tops[r], ctxs[t], state, p, rt, targets,
                     calibratedMode_);
        }
      } catch (...) {
        std::lock_guard g(exMtx);
        if (!wex) wex = std::current_exception();
      }
    });
  }
  for (auto& th : threads) th.join();
  if (wex) std::rethrow_exception(wex);

  {
    size_t withIdx = 0, withName = 0, withMc = 0;
    for (auto& [key, ki] : state.table) {
      if (ki.targetIdx >= 0) ++withIdx;
      if (!ki.name.empty()) ++withName;
      if (ki.mc) ++withMc;
    }
    spdlog::debug("heap: klass table {} entries, named {}, mc {}, with targetIdx {}",
                  state.table.size(), withName, withMc, withIdx);
  }

  // Zero-target diagnosis: the runtime class-naming scheme may not match the
  // mapping table at all (e.g. Forge/NeoForge use official Mojang names while
  // Fabric uses intermediary class_XXXX). Log a sample of the MC-named klasses
  // and dump the well-known client singleton so the mismatch is visible in one
  // run.
  {
    bool anyHit = false;
    for (auto& [name, cnt] : r.targetCounts)
      if (cnt > 0) { anyHit = true; break; }
    if (!anyHit) {
      static const char* kProbeNames[] = {
          "net/minecraft/client/Minecraft", // Forge/NeoForge runtime (official)
          "net/minecraft/class_310",        // Fabric/vanilla (intermediary)
      };
      size_t logged = 0;
      uint64_t probe = 0;
      for (auto& [key, ki] : state.table) {
        if (!ki.mc) continue;
        if (logged < 20) {
          spdlog::debug("heap: mc klass sample '{}' @ {}", ki.name, util::hex(key));
          ++logged;
        }
        for (const char* pn : kProbeNames)
          if (ki.name == pn) probe = key;
      }
      if (probe) {
        KlassReader kr;
        kr.dumpClass(p, rt, probe, "probe");
      }
      // java/lang/String has a tiny well-known layout (value@16, hash@20,
      // coder@24, hashIsZero@25 on x64) — a perfect reference for locking the
      // legacy FieldInfo offset packing.
      for (auto& [key, ki] : state.table) {
        if (ki.name == "java/lang/String") {
          KlassReader kr;
          kr.dumpClass(p, rt, key, "String-ref");
          break;
        }
      }
    }
  }

  for (auto& c : ctxs) {
    r.objectsSeen += c.objects;
    r.mcObjects += c.mc;
    r.regionsScanned += c.regions;
    for (size_t i = 0; i < targets.size(); ++i) {
      r.targetCounts[targets[i]] += c.counts[i];
      auto& dst = r.targetAddrs[targets[i]];
      for (uint64_t a : c.addrs[i]) {
        if (dst.size() >= kMaxAddrsPerTarget) break;
        dst.push_back(a);
      }
    }
  }
  for (auto& [name, cnt] : r.targetCounts)
    spdlog::debug("heap: target {} = {} (addrs {})", name, cnt,
                  r.targetAddrs[name].size());

  if (r.objectsSeen == 0 && fallbackReady_) {
    // Linear scan over the reserved heap range, page-granular on holes.
    r.regionsScanned = 0;
    WalkCtx ctx;
    ctx.local.reserve(1 << 16);
    ctx.addrs.resize(targets.size());
    ctx.counts.assign(targets.size(), 0);
    for (uint64_t a = heapStart_; a < heapEnd_; a += kFallbackChunk) {
      if (cancel && cancel->load()) break;
      uint64_t n = std::min<uint64_t>(kFallbackChunk, heapEnd_ - a);
      auto buf = p.readBytes(a, (size_t)n);
      if (buf.empty()) {
        for (uint64_t pa = a; pa < a + n; pa += 4096) {
          auto page = p.readBytes(pa, 4096);
          if (page.empty()) continue;
          walkRegion(pa, pa + 4096, ctx, state, p, rt, targets, calibratedMode_);
        }
        continue;
      }
      walkRegion(a, a + buf.size(), ctx, state, p, rt, targets, calibratedMode_);
    }
    r.objectsSeen += ctx.objects;
    r.mcObjects += ctx.mc;
    r.regionsScanned += ctx.regions;
    for (size_t i = 0; i < targets.size(); ++i) {
      r.targetCounts[targets[i]] += ctx.counts[i];
      auto& dst = r.targetAddrs[targets[i]];
      for (uint64_t a : ctx.addrs[i]) {
        if (dst.size() >= kMaxAddrsPerTarget) break;
        dst.push_back(a);
      }
    }
  }

  auto t1 = std::chrono::steady_clock::now();
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.ok = r.objectsSeen > 0;
  if (!r.ok && r.error.empty()) r.error = "no objects found";
  return r;
}

std::string HeapReader::regionSummary(const mem::Process& p, const JvmRuntime& rt,
                                      uint64_t addr) const {
  if (!addr) return "addr=0";
  RegionList rl = collectRegions(p, rt);
  if (!rl.ok) return "regions unavailable (" + rl.error + ")";
  auto plausible = [](uint32_t t) { return t == 2 || t == 3 || t == 8; };
  for (size_t i = 0; i < rl.size(); ++i) {
    if (addr >= rl.bottoms[i] && addr < rl.tops[i]) {
      return std::format("region {}/{} [{} .. {}] tag={:X}{}", i + 1, rl.size(),
                         util::hex(rl.bottoms[i]), util::hex(rl.tops[i]), rl.tags[i],
                         plausible(rl.tags[i]) ? " (walk covers it)" : " (SKIPPED by walk)");
    }
  }
  return "not inside any listed region";
}

} // namespace jvm
