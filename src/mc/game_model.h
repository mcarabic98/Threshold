#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "jvm/heap_reader.h"
#include "jvm/jvm_runtime.h"
#include "jvm/klass_reader.h"
#include "mc/mappings.h"
#include "memory/process.h"

namespace mc {

struct EntitySnapshot {
  bool valid = false;
  std::string error;
  double x = 0, y = 0, z = 0;
  float yaw = 0, pitch = 0;
  uint64_t playerObj = 0;
};

// One ESP target read from the live heap.
enum class EntCat : int { Mob = 0, Player, ArmorStand, Projectile, Item, Living, Misc, Count };

struct EspEntity {
  uint64_t obj = 0;
  std::string className; // intermediary class id, e.g. net/minecraft/class_1642
  int category = 5;      // EntCat
  bool self = false;     // local player
  double cur[3] = {0, 0, 0};  // current tick position (Vec3)
  double prev[3] = {0, 0, 0}; // previous tick position (xo/yo/zo)
  double bbMin[3] = {0, 0, 0};
  double bbMax[3] = {0, 0, 0};
};

struct CameraSnapshot {
  bool valid = false;
  std::string error;
  double pos[3] = {0, 0, 0};
  float forwards[3] = {0, 0, 0};
  float up[3] = {0, 0, 0};
  float left[3] = {0, 0, 0};
};

// Navigates the live object graph externally:
//   class_310 (Minecraft singleton) --field_1724--> class_746 (LocalPlayer)
//   --super chain--> class_1297 (Entity) position fields.
//   class_310 -> level -> entity lookup maps -> all live entities.
//   class_310 -> gameRenderer -> camera (position + basis vectors).
// All reads; field offsets are parsed from InstanceKlass::_fieldinfo_stream.
class GameModel {
public:
  bool init(const mem::Process& p, jvm::JvmRuntime& rt, jvm::KlassReader& kr,
            const jvm::HeapScanResult& scan, uint64_t heapLo, uint64_t heapHi,
            const MappingSet& map);
  bool valid() const { return valid_; }

  bool readPlayer(const mem::Process& p, EntitySnapshot& out);
  bool readCamera(const mem::Process& p, CameraSnapshot& out);
  bool readEntities(const mem::Process& p, std::vector<EspEntity>& out);
  // Tick-interpolation phase in [0, 1]: fraction of the current 50 ms tick
  // elapsed, estimated from observed entity position changes. Renderers use
  // it exactly like the game's partialTick.
  double tickPartial() const;
  float readFov(const mem::Process& p);       // degrees, 70 fallback (heap chain)
  float readFovFromFile(const mem::Process& p); // degrees from options.txt (PEB cwd)
  void setGameVersion(const std::string& v) { gameVersion_ = v; }
  bool isEntitySubclass(const mem::Process& p, uint64_t klass);
  int classifyEntity(const mem::Process& p, uint64_t klass);
  std::map<uint64_t, int> categoryCache_;

  // Diagnostics: log fields of an object's class, with live values and the
  // class names of referenced objects (one level deep per call).
  void dumpObject(const mem::Process& p, jvm::JvmRuntime& rt, jvm::KlassReader& kr,
                  uint64_t obj, const std::string& label);

  // Navigation helpers used by dumpObject and the entity scan.
  std::optional<uint64_t> objectField(const mem::Process& p, uint64_t obj,
                                      const jvm::FieldDesc& f);
  uint64_t klassOf(const mem::Process& p, uint64_t obj) const;
  std::string klassName(const mem::Process& p, uint64_t klass);
  bool readVec3(const mem::Process& p, uint64_t obj, int64_t off, double out[3]);

private:
  const std::vector<jvm::FieldDesc>& fields(const mem::Process& p, jvm::JvmRuntime& rt,
                                            jvm::KlassReader& kr, uint64_t klass);
  jvm::KlassReader* kr_ = nullptr;
  std::optional<int64_t> fieldOffset(const std::vector<jvm::FieldDesc>& fs,
                                     const std::string& name, const std::string& desc);
  std::optional<int64_t> fieldOffsetByDesc(const std::vector<jvm::FieldDesc>& fs,
                                           const std::string& desc);
  // BFS from the ClientLevel object collecting the Int2ObjectMap entity maps
  // (path to them differs per version/loader — see the .cpp comment).
  std::vector<uint64_t> findEntityMaps(const mem::Process& p, uint64_t level);
  uint64_t decodeOop(uint64_t raw) const;

  bool valid_ = false;
  const mem::Process* p_ = nullptr;
  jvm::JvmRuntime* rt_ = nullptr;
  uint64_t mcInstance_ = 0;
  int64_t offPlayer_ = -1;      // field_1724 (class_746) inside class_310
  int64_t offPositionVec_ = -1; // field_22467 (Vec3 position) inside class_1297
  int64_t offYaw_ = -1, offPitch_ = -1;
  int64_t vecX_ = 16, vecY_ = 24, vecZ_ = 32; // Vec3.x/y/z
  int64_t offLevel_ = -1;
  int64_t offGameRenderer_ = -1;
  int64_t offCamera_ = -1;         // camera inside gameRenderer
  int64_t offEntitiesLookup_ = -1; // entity lookup holder inside clientLevel
  int64_t offCamPos_ = -1, offCamFwd_ = -1, offCamUp_ = -1, offCamLeft_ = -1;
  int64_t vec3fX_ = -1, vec3fY_ = -1, vec3fZ_ = -1; // Vector3f floats
  int64_t offBB_ = -1;                              // boundingBox (AABB) inside entity
  int64_t aabbMinX_ = -1;                           // AABB: 6 doubles from here
  int64_t prevX_ = -1, prevY_ = -1, prevZ_ = -1;    // xo/yo/zo offsets in Entity
  int64_t offOptions_ = -1;                         // Options inside class_310

  // Per-entity tick interpolation: prev = our previous-frame cur (exactly the
  // game's xo/yo/zo semantics), phase from the latest observed position
  // change (entity ticks are synchronized across the world).
  struct EntTick {
    bool has = false;
    double cur[3] = {0, 0, 0};
    double prev[3] = {0, 0, 0};
  };
  std::map<uint64_t, EntTick> entTick_;
  std::chrono::steady_clock::time_point lastTickChange_{};
  int64_t offFovOpt_ = -1;                          // fov OptionInstance inside Options
  int64_t offOptValue_ = -1;                        // OptionInstance.value (Object)
  int64_t doubleValue_ = -1;                        // java/lang/Double.value (D)
  uint64_t entityKlass_ = 0;                        // class_1297 InstanceKlass*
  std::string gameVersion_;                         // MC version from window title

  // heap oop decode
  bool narrowOops_ = true;
  uint64_t oopBase_ = 0;
  int oopShift_ = 0;
  std::map<uint64_t, std::vector<jvm::FieldDesc>> fieldCache_;
  bool graphDumped_ = false;
};

} // namespace mc
