#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jvm/jvm_env.h"
#include "jvm/jvm_runtime.h"
#include "memory/process.h"

namespace jvm {

struct HeapScanResult {
  bool ok = false;
  std::string error;
  size_t objectsSeen = 0;
  size_t regionsScanned = 0;
  double seconds = 0;
  size_t mcObjects = 0; // "net/minecraft/" prefix hits
  std::map<std::string, size_t> targetCounts;
  std::map<std::string, std::vector<uint64_t>> targetAddrs; // first 16 instances each
  std::map<std::string, uint64_t> targetKlass; // InstanceKlass* per matched target name
};

// Walks the live Java heap externally — G1 region list via VMStructs, with a
// linear-scan fallback over the reserved heap range. Read-only; a scan is a
// weakly consistent snapshot (GC may skew counts slightly). The walk is
// parallelized over regions; per-region cancel checks allow prompt aborts.
class HeapReader {
public:
  bool resolve(const mem::Process& p, JvmRuntime& rt);
  bool ready() const { return ready_; }
  bool g1() const { return g1Ready_; }
  int calibratedMode() const { return calibratedMode_; }
  uint64_t heapStart() const { return heapStart_; }
  uint64_t heapEnd() const { return heapEnd_; }

  HeapScanResult scan(const mem::Process& p, const JvmRuntime& rt,
                      const std::vector<std::string>& targets,
                      std::atomic<bool>* cancel = nullptr);

  // One-shot diagnostic: which listed region holds addr, its HeapRegionType
  // tag, and whether the walk covers that tag. Empty list / error reported
  // as text. Used when a target class unexpectedly has zero instances.
  std::string regionSummary(const mem::Process& p, const JvmRuntime& rt,
                            uint64_t addr) const;

private:
  struct KlassInfo {
    bool valid = false;
    bool mc = false;       // net/minecraft/ prefix
    int targetIdx = -1;    // index into the target list, -1 = none
    uint32_t lh = 0;
    std::string name;
  };
  // Key = decoded Klass* address. Hot table; kept reserved to avoid rehash.
  using KlassCache = std::unordered_map<uint64_t, KlassInfo>;

  struct WalkCtx {
    KlassCache local;
    std::vector<std::vector<uint64_t>> addrs; // per target
    std::vector<uint64_t> klassAddr;          // per target: InstanceKlass* (first hit)
    std::vector<size_t> counts;               // per target
    size_t objects = 0, mc = 0, regions = 0, resync = 0, named = 0;
  };
  struct ScanState {
    std::mutex mtx;
    KlassCache table;
  };

  const KlassInfo& resolve(uint64_t key, WalkCtx& ctx, ScanState& st, const mem::Process& p,
                           const JvmRuntime& rt, const std::vector<std::string>& targets) const;
  void walkRegion(uint64_t bottom, uint64_t top, WalkCtx& ctx, ScanState& st,
                  const mem::Process& p, const JvmRuntime& rt,
                  const std::vector<std::string>& targets, int sizeMode) const;

  const KlassInfo& klassInfo(const mem::Process& p, const JvmRuntime& rt, uint64_t klassAddr,
                             KlassCache& cache) const;

  // Region list access (shared by scan / diagnostics / base derivation).
  struct RegionList {
    bool ok = false;
    std::string error;
    std::vector<uint64_t> bottoms;
    std::vector<uint64_t> tops;
    std::vector<uint32_t> tags; // HeapRegionType tag per span
    size_t size() const { return bottoms.size(); }
  };
  RegionList collectRegions(const mem::Process& p, const JvmRuntime& rt) const;
  // One candidate region-table read (embedded or pointer _hrm layout).
  RegionList collectRegionsHrm(uint64_t hrm, const mem::Process& p,
                               const JvmRuntime& rt) const;
  // CollectedHeap::_reserved as [start, end). JDK 21 publishes the size in
  // HeapWords (MemRegion::_word_size); older JDKs are parsed by shape.
  std::optional<std::pair<uint64_t, uint64_t>> reservedRange(const mem::Process& p,
                                                             const JvmRuntime& rt) const;
  std::map<uint64_t, size_t> sampleKlassIds(const mem::Process& p, const RegionList& rl,
                                            int maxRegions) const;

  // JDK 21+ removed Universe::_narrow_klass_* from VMStructs. Derive the
  // decode empirically: vmClasses::_klasses[] gives true Klass* addresses of
  // fundamental classes; heap samples give narrow ids; solve for base+shift.
  bool deriveKlassDecode(const mem::Process& p, JvmRuntime& rt);

  uint64_t collectedHeap_ = 0;
  bool ready_ = false;
  mutable int calibratedMode_ = 0; // 0 = use env default
  mutable bool rlDumped_ = false;  // one-shot region-struct diagnostics
  mutable bool rlHistDumped_ = false;

  // G1 path (offsets inside the target process)
  int64_t offG1Hrm_ = -1;        // G1CollectedHeap::_hrm
  int64_t offHrmRegions_ = -1;   // HeapRegionManager::_regions
  int64_t offGarrData_ = -1;     // GrowableArray<T>::_data
  int64_t offGarrLen_ = -1;      // GrowableArrayBase::_len
  int64_t offRegionBottom_ = -1; // HeapRegion::_bottom
  int64_t offRegionTop_ = -1;    // HeapRegion::_top
  bool g1Ready_ = false;

  // fallback path
  uint64_t heapStart_ = 0;
  uint64_t heapEnd_ = 0;
  bool fallbackReady_ = false;
};

} // namespace jvm
