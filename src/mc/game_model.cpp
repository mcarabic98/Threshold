#include "mc/game_model.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <fstream>
#include <set>
#include <cstring>

#include "core/util.h"

namespace mc {

using jvm::FieldDesc;
using jvm::JvmEnv;

std::optional<int64_t> GameModel::fieldOffset(const std::vector<FieldDesc>& fs,
                                              const std::string& name,
                                              const std::string& desc) {
  if (!name.empty())
    for (auto& f : fs)
      if (!f.isStatic && f.name == name && f.descriptor == desc) return (int64_t)f.offset;
  // Nameless rules (SRG id unknown) and renamed fields: a unique descriptor
  // match is still a safe resolution; ambiguous ones need caller validation.
  const FieldDesc* hit = nullptr;
  int n = 0;
  for (auto& f : fs)
    if (!f.isStatic && f.descriptor == desc) {
      hit = &f;
      ++n;
    }
  if (n == 1) return (int64_t)hit->offset;
  return std::nullopt;
}

std::optional<int64_t> GameModel::fieldOffsetByDesc(const std::vector<FieldDesc>& fs,
                                                    const std::string& desc) {
  for (auto& f : fs)
    if (!f.isStatic && f.descriptor == desc) return (int64_t)f.offset;
  return std::nullopt;
}

uint64_t GameModel::klassOf(const mem::Process& p, uint64_t obj) const {
  auto kid = p.read<uint32_t>(obj + 8);
  if (!kid) return 0;
  const JvmEnv& env = rt_->env();
  return env.compressedKlasses ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                               : (uint64_t)*kid;
}

std::string GameModel::klassName(const mem::Process& p, uint64_t klass) {
  const JvmEnv& env = rt_->env();
  auto sym = p.read<uint64_t>((uintptr_t)(klass + env.offKlassName));
  if (!sym || !*sym) return "?";
  return rt_->readSymbol(p, *sym).value_or("?");
}

bool GameModel::readVec3(const mem::Process& p, uint64_t obj, int64_t off, double out[3]) {
  if (off < 0) return false;
  auto raw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)off));
  if (!raw || *raw == 0) return false;
  uint64_t vec = decodeOop(*raw);
  for (int i = 0; i < 3; ++i) {
    auto v = p.read<double>((uintptr_t)(vec + (uint64_t)vecX_ + (uint64_t)i * 8));
    if (!v) return false;
    out[i] = *v;
  }
  return true;
}

const std::vector<FieldDesc>& GameModel::fields(const mem::Process& p, jvm::JvmRuntime& rt,
                                                jvm::KlassReader& kr, uint64_t klass) {
  auto it = fieldCache_.find(klass);
  if (it != fieldCache_.end()) return it->second;
  std::vector<FieldDesc> fs;
  std::string err;
  if (!kr.readFields(p, rt, klass, fs, &err))
    spdlog::debug("model: fields of {} failed: {}", util::hex(klass), err);
  return fieldCache_.emplace(klass, std::move(fs)).first->second;
}

uint64_t GameModel::decodeOop(uint64_t raw) const {
  if (!narrowOops_) return raw;
  return oopBase_ + (raw << oopShift_);
}

bool GameModel::init(const mem::Process& p, jvm::JvmRuntime& rt, jvm::KlassReader& kr,
                     const jvm::HeapScanResult& scan, uint64_t heapLo, uint64_t heapHi,
                     const MappingSet& map) {
  valid_ = false;
  mcInstance_ = 0;
  offPlayer_ = -1;
  offPositionVec_ = -1;
  offYaw_ = offPitch_ = -1;

  auto mcName = map.className("minecraft");
  auto playerClass = map.className("localPlayer");
  auto entityClass = map.className("entity");
  const FieldRule* playerRule = map.field("player");
  const FieldRule* posRule = map.field("position");
  const FieldRule* yawRule = map.field("yRot");
  const FieldRule* xRotRule = map.field("xRot");
  if (!mcName || !playerClass || !entityClass || !playerRule || !posRule) {
    spdlog::warn("model: mapping incomplete (classes/fields)");
    return false;
  }

  auto it = scan.targetAddrs.find(*mcName);
  if (it == scan.targetAddrs.end() || it->second.empty()) return false;
  mcInstance_ = it->second.front();

  const JvmEnv& env = rt.env();

  // Minecraft instance -> klass -> field table -> player offset.
  auto id = p.read<uint32_t>(mcInstance_ + 8);
  if (!id) return false;
  uint64_t mcKlass =
      env.compressedKlasses ? env.narrowKlassBase + ((uint64_t)*id << env.narrowKlassShift) : *id;
  auto& mcFields = fields(p, rt, kr, mcKlass);
  auto playerOff = fieldOffset(mcFields, playerRule->name, playerRule->type);
  if (!playerOff) {
    spdlog::warn("model: {} ({}) not found in {}", playerRule->name, playerRule->type, *mcName);
    return false;
  }
  offPlayer_ = *playerOff;

  // Read the player oop and derive the heap-oop decode from it: the decoded
  // object's klass id must equal the klass id of a known player instance.
  auto knownIt = scan.targetAddrs.find(*playerClass);
  uint32_t expectedKlassId = 0;
  if (knownIt != scan.targetAddrs.end() && !knownIt->second.empty()) {
    if (auto kid = p.read<uint32_t>(knownIt->second.front() + 8)) expectedKlassId = *kid;
  }

  auto rawOop = p.read<uint32_t>((uintptr_t)(mcInstance_ + offPlayer_));
  if (!rawOop || *rawOop == 0) {
    spdlog::warn("model: player oop is null (main menu?)");
    return false;
  }

  struct Hypo {
    uint64_t base;
    int shift;
  };
  std::vector<Hypo> hypos = {{0, 3}, {heapLo, 0}, {heapLo, 3}};
  if (env.compressedOops && (env.narrowOopBase != 0 || env.narrowOopShift != 0))
    hypos.insert(hypos.begin(), {env.narrowOopBase, env.narrowOopShift});

  uint64_t playerObj = 0;
  for (auto& h : hypos) {
    uint64_t addr = h.base + ((uint64_t)*rawOop << h.shift);
    if (heapLo && (addr < heapLo || addr >= heapHi)) continue;
    auto kid = p.read<uint32_t>(addr + 8);
    if (!kid || (expectedKlassId && *kid != expectedKlassId)) continue;
    playerObj = addr;
    rt.setNarrowOop(h.base, h.shift);
    narrowOops_ = true;
    oopBase_ = h.base;
    oopShift_ = h.shift;
    spdlog::info("model: heap oop decode base={} shift={} (player @ {})", util::hex(h.base),
                 h.shift, util::hex(playerObj));
    break;
  }
  if (!playerObj) {
    spdlog::warn("model: no heap-oop hypothesis matched (raw=0x{:X})", *rawOop);
    return false;
  }

  // Walk the super chain to the Entity class; locate position/rotation by
  // the mapping rules.
  uint64_t k = playerObj ? [&] {
    auto kid = p.read<uint32_t>(playerObj + 8);
    return env.compressedKlasses ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                                 : (uint64_t)*kid;
  }() : 0;

  int depth = 0;
  bool entityFound = false;
  std::vector<int64_t> posCandidates; // Vec3-typed fields of Entity, sorted
  std::vector<int64_t> floatOffs;     // float fields of Entity, sorted
  while (k && depth++ < 12) {
    auto& fs = fields(p, rt, kr, k);
    auto nmSym = p.read<uint64_t>((uintptr_t)(k + env.offKlassName));
    std::string kname = nmSym && *nmSym ? (rt.readSymbol(p, *nmSym).value_or("?")) : "?";
    spdlog::debug("model: chain {} '{}'", util::hex(k), kname);

    if (kname == *entityClass) {
      entityFound = true;
      entityKlass_ = k;
      // Entity holds several Vec3s (position, deltaMovement, ...) and dozens
      // of floats (yRot/xRot among them) — collect candidates now, resolve
      // after the camera is known (position/rotation validate against it).
      if (auto named = fieldOffset(fs, posRule->name, posRule->type))
        offPositionVec_ = *named;
      for (auto& f : fs) {
        if (f.isStatic) continue;
        if (f.descriptor == posRule->type) posCandidates.push_back((int64_t)f.offset);
        if (f.descriptor == "F") floatOffs.push_back((int64_t)f.offset);
      }
      break;
    }
    auto sup = kr.superClass(p, rt, k);
    if (!sup || !*sup) break;
    k = *sup;
  }
  if (!entityFound) {
    spdlog::warn("model: class_1297 not reached via super chain");
    return false;
  }

  // Vec3.x/y/z offsets from class_243's own field table (first three doubles).
  if (auto vecRaw = p.read<uint32_t>((uintptr_t)(mcInstance_ + offPlayer_)); vecRaw && *vecRaw) {
    uint64_t player2 = decodeOop(*vecRaw);
    if (auto kid = p.read<uint32_t>(player2 + 8)) {
      uint64_t pk = env.compressedKlasses
                        ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                        : (uint64_t)*kid;
      for (auto& f : fields(p, rt, kr, pk)) {
        if (f.isStatic || f.descriptor != "D") continue;
        if (vecX_ < 0) vecX_ = (int64_t)f.offset;
        else if (vecY_ < 0) vecY_ = (int64_t)f.offset;
        else if (vecZ_ < 0) vecZ_ = (int64_t)f.offset;
      }
      if (vecX_ < 0) vecX_ = 16, vecY_ = 24, vecZ_ = 32;
    }
  }

  valid_ = true;
  p_ = &p;
  rt_ = &rt;
  kr_ = &kr;

  // ESP offsets: level / gameRenderer / camera / entity lookup / AABB.
  // Descriptors are built from the mapping's own class names — the runtime
  // naming scheme differs per loader (intermediary vs official Mojang).
  auto descOf = [&](const char* key) -> std::string {
    auto n = map.className(key);
    return n ? "L" + *n + ";" : std::string();
  };
  std::string descLevel = descOf("clientLevel");
  std::string descGR = descOf("gameRenderer");
  std::string descCam = descOf("camera");
  std::string descVec3 = descOf("vec3");
  std::string descAABB = descOf("aabb");
  auto byName = [](const std::vector<FieldDesc>& fs,
                   const std::string& name) -> const FieldDesc* {
    for (auto& f : fs)
      if (!f.isStatic && f.name == name) return &f;
    return nullptr;
  };
  auto ruleName = [&](const char* key) -> std::string {
    auto r = map.field(key);
    return r ? r->name : std::string();
  };
  auto ruleType = [&](const char* key) -> std::string {
    auto r = map.field(key);
    return r && !r->type.empty() ? r->type : std::string();
  };
  auto& mcFs = fields(p, rt, kr, mcKlass);
  spdlog::debug("model: esp: mcKlass={} mcFs={} fields", util::hex(mcKlass), mcFs.size());

  // level: rule name first ("field_1687" / "f_91073_"), then unique type.
  if (auto lo = fieldOffset(mcFs, ruleName("level"),
                            ruleType("level").empty() ? descLevel : ruleType("level")))
    offLevel_ = *lo;
  else if (!descLevel.empty())
    offLevel_ = fieldOffset(mcFs, {}, descLevel).value_or(-1);
  // gameRenderer: same pattern.
  if (auto go = fieldOffset(mcFs, ruleName("gameRenderer"),
                            ruleType("gameRenderer").empty() ? descGR : ruleType("gameRenderer")))
    offGameRenderer_ = *go;
  else if (!descGR.empty())
    offGameRenderer_ = fieldOffset(mcFs, {}, descGR).value_or(-1);
  if (offGameRenderer_ < 0)
    spdlog::warn("model: esp: gameRenderer field not found ({} fields)", mcFs.size());

  auto objAt = [&](uint64_t obj, int64_t off) -> std::optional<uint64_t> {
    if (off < 0 || !obj) return std::nullopt;
    if (auto raw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)off)); raw && *raw)
      return decodeOop(*raw);
    return std::nullopt;
  };
  if (uint64_t grObj = 0; true) {
    if (offGameRenderer_ >= 0) {
      if (auto o = objAt(mcInstance_, offGameRenderer_)) grObj = *o;
    } else if (auto grFd = byName(mcFields, ruleName("gameRenderer")); grFd) {
      if (auto o = objectField(p, mcInstance_, *grFd)) grObj = *o;
    }
    if (grObj) {
      uint64_t grKlass = klassOf(p, grObj);
      auto& grFs = fields(p, rt, kr, grKlass);
      // camera: unique Camera-typed field of GameRenderer (SRG id varies).
      std::string camDesc = ruleType("camera").empty() ? descCam : ruleType("camera");
      if (auto camFd = fieldOffset(grFs, ruleName("camera"), camDesc)) {
        if (auto camObj = objAt(grObj, *camFd)) {
          uint64_t camKlass = klassOf(p, *camObj);
          auto& camFs = fields(p, rt, kr, camKlass);
          if (!descVec3.empty())
            if (auto o = fieldOffsetByDesc(camFs, descVec3)) offCamPos_ = *o;
          int seen = 0;
          for (auto& f : camFs) {
            if (f.isStatic || f.descriptor != "Lorg/joml/Vector3f;") continue;
            if (seen == 0) offCamFwd_ = (int64_t)f.offset;
            if (seen == 1) offCamUp_ = (int64_t)f.offset;
            if (seen == 2) offCamLeft_ = (int64_t)f.offset;
            ++seen;
          }
          // Vector3f component floats: from the first Vector3f object (the
          // camera forwards) — its class has exactly three float fields.
          for (auto& f : camFs) {
            if (f.isStatic || f.descriptor != "Lorg/joml/Vector3f;") continue;
            if (auto vf = objectField(p, *camObj, f)) {
              uint64_t vk = klassOf(p, *vf);
              int seen2 = 0;
              for (auto& g : fields(p, rt, kr, vk)) {
                if (g.isStatic || g.descriptor != "F") continue;
                if (seen2 == 0) vec3fX_ = (int64_t)g.offset;
                if (seen2 == 1) vec3fY_ = (int64_t)g.offset;
                if (seen2 == 2) vec3fZ_ = (int64_t)g.offset;
                ++seen2;
              }
            }
            break;
          }
        }
        offCamera_ = *camFd;
      } else {
        spdlog::warn("model: esp: camera field not found in GameRenderer");
      }
    }
  }

  // Resolve the ambiguous Entity fields now that the camera is known: the
  // player position shares x/z with the camera position (first person), and
  // the player yaw/pitch match the camera rotation derived from its forwards
  // vector. Without a usable camera (main menu) fall back to first candidates.
  auto readD3 = [&](uint64_t obj, int64_t base, double out[3]) {
    for (int c = 0; c < 3; ++c) {
      auto v = p.read<double>((uintptr_t)(obj + (uint64_t)(base + (int64_t)c * 8)));
      if (!v) return false;
      out[c] = *v;
    }
    return true;
  };
  auto readF3 = [&](uint64_t obj, int64_t base, float out[3]) {
    for (int c = 0; c < 3; ++c) {
      auto v = p.read<float>((uintptr_t)(obj + (uint64_t)(base + (int64_t)c * 4)));
      if (!v) return false;
      out[c] = *v;
    }
    return true;
  };
  double camPos[3] = {0, 0, 0};
  float camFwd[3] = {0, 0, 0};
  bool haveCam = false;
  if (auto camObj = objAt(objAt(mcInstance_, offGameRenderer_).value_or(0), offCamera_);
      camObj && offCamPos_ >= 0 && offCamFwd_ >= 0 && vecX_ >= 0 && vec3fX_ >= 0) {
    haveCam = readD3(*camObj, offCamPos_, camPos) && readF3(*camObj, offCamFwd_, camFwd);
  }
  if (offPositionVec_ < 0 && !posCandidates.empty()) {
    for (auto off : posCandidates) {
      if (auto po = objAt(playerObj, off)) {
        double v[3];
        if (readD3(*po, vecX_, v) && haveCam) {
          if (std::abs(v[0] - camPos[0]) < 32.0 && std::abs(v[2] - camPos[2]) < 32.0) {
            offPositionVec_ = off;
            break;
          }
        }
      }
    }
    if (offPositionVec_ < 0) offPositionVec_ = posCandidates.front();
  }
  if (!floatOffs.empty()) {
    // MC rotation from the camera forwards vector: yaw = atan2(-fx, fz),
    // pitch = -asin(fy) (positive pitch looks down).
    double yaw = 0, pitch = 0;
    bool haveRot = false;
    if (haveCam) {
      double fy = (double)camFwd[1];
      yaw = std::atan2(-(double)camFwd[0], (double)camFwd[2]) * 180.0 / 3.14159265358979;
      pitch = std::asin(std::clamp(fy, -1.0, 1.0)) * -180.0 / 3.14159265358979;
      haveRot = true;
    }
    auto wrapDeg = [](double d) {
      while (d > 180.0) d -= 360.0;
      while (d < -180.0) d += 360.0;
      return d;
    };
    for (size_t i = 0; i + 1 < floatOffs.size(); ++i) {
      int64_t a = floatOffs[i], b = floatOffs[i + 1];
      if (b - a != 4) continue;
      if (haveRot) {
        auto ya = p.read<float>((uintptr_t)(playerObj + (uint64_t)a));
        auto xb = p.read<float>((uintptr_t)(playerObj + (uint64_t)b));
        if (!ya || !xb) continue;
        if (std::abs(wrapDeg((double)*ya - yaw)) < 4.0 &&
            std::abs((double)*xb - pitch) < 4.0) {
          offYaw_ = a;
          offPitch_ = b;
          break;
        }
      }
    }
    if (offYaw_ < 0) {
      // first adjacent float pair as best-effort fallback
      for (size_t i = 0; i + 1 < floatOffs.size(); ++i) {
        if (floatOffs[i + 1] - floatOffs[i] == 4) {
          offYaw_ = floatOffs[i];
          offPitch_ = floatOffs[i + 1];
          break;
        }
      }
    }
  }
  // Entity boundingBox (AABB): field by descriptor on the entity class, plus
  // the six double offsets inside the AABB class.
  {
    auto& entFs = fields(p, rt, kr, entityKlass_);
    if (!descAABB.empty()) {
      if (auto o = fieldOffsetByDesc(entFs, descAABB)) offBB_ = *o;
      if (offBB_ < 0 && entityKlass_) {
        uint64_t kk = entityKlass_;
        for (int d = 0; d < 12 && kk && offBB_ < 0; ++d) {
          auto& fs2 = fields(p, rt, kr, kk);
          if (auto o = fieldOffsetByDesc(fs2, descAABB)) {
            offBB_ = *o;
            break;
          }
          auto sup = kr.superClass(p, rt, kk);
          if (!sup || !*sup) break;
          kk = *sup;
        }
      }
    }
    if (offBB_ >= 0) {
      auto bbObj = [&] {
        auto raw = p.read<uint32_t>((uintptr_t)(playerObj + (uint64_t)offBB_));
        return raw && *raw ? decodeOop(*raw) : 0;
      }();
      if (bbObj) {
        uint64_t bk = klassOf(p, bbObj);
        int seen = 0;
        for (auto& f : fields(p, rt, kr, bk)) {
          if (f.isStatic || f.descriptor != "D") continue;
          if (seen == 0) aabbMinX_ = (int64_t)f.offset;
          ++seen;
        }
      }
    }
    // xo/yo/zo (prev tick pos): first three D fields of the Entity class.
    if (entityKlass_) {
      auto& entFs2 = fields(p, rt, kr, entityKlass_);
      int dn = 0;
      for (auto& f : entFs2) {
        if (f.isStatic || f.descriptor != "D") continue;
        if (dn == 0) prevX_ = (int64_t)f.offset;
        if (dn == 1) prevY_ = (int64_t)f.offset;
        if (dn == 2) prevZ_ = (int64_t)f.offset;
        ++dn;
      }
    }
    // FOV chain: mc.options (class_315) -> enumerate ALL fields -> find the
    //   one whose value (Double via OptionInstance or raw double) is in the
    //   FOV degree range [30, 110]. Try field_1842 by name first, then scan.
    const FieldDesc* optF = nullptr;
    for (auto& f : mcFs)
      if (!f.isStatic && f.descriptor == "Lnet/minecraft/class_315;") optF = &f;
    if (optF) {
      offOptions_ = (int64_t)optF->offset;
      if (auto optObj = objectField(p, mcInstance_, *optF)) {
        // Diagnostic: log field_1842 in Options (fov candidate)
        {
          uint64_t optKlass0 = klassOf(p, *optObj);
          for (auto& f : fields(p, rt, kr, optKlass0)) {
            if (f.name != "field_1842") continue;
            spdlog::debug("fov: field_1842 {} @ {} static={}", f.descriptor, f.offset, f.isStatic);
            if (f.descriptor == "D" && !f.isStatic) {
              if (auto v = p.read<double>((uintptr_t)(*optObj + f.offset)))
                spdlog::debug("fov: field_1842 value = {:.3f}", *v);
            }
          }
        }
        uint64_t optKlass = klassOf(p, *optObj);
        auto& optFs = fields(p, rt, kr, optKlass);
        int64_t oiValueOff = -1;
        for (auto& f : optFs) {
          if (f.isStatic || f.descriptor.find("class_7172") == std::string::npos) continue;
          if (auto oi = objectField(p, *optObj, f)) {
            uint64_t oiK = klassOf(p, *oi);
            for (auto& of : fields(p, rt, kr, oiK))
              if (!of.isStatic && of.descriptor == "Ljava/lang/Object;" && oiValueOff < 0)
                oiValueOff = (int64_t)of.offset;
            break;
          }
        }
        spdlog::debug("fov: OptionInstance value offset {}", oiValueOff);
        if (oiValueOff >= 0) {
          // resolve java/lang/Double.value offset (the boxed double)
          if (doubleValue_ < 0) {
            // find a Double object via some OptionInstance's value
            for (auto& f : optFs) {
              if (f.isStatic || f.descriptor.find("class_7172") == std::string::npos) continue;
              auto oiRaw = p.read<uint32_t>((uintptr_t)(*optObj + (uint64_t)f.offset));
              if (!oiRaw || !*oiRaw) continue;
              uint64_t oi = decodeOop(*oiRaw);
              auto vRaw = p.read<uint32_t>((uintptr_t)(oi + (uint64_t)oiValueOff));
              if (!vRaw || !*vRaw) continue;
              uint64_t dbl = decodeOop(*vRaw);
              uint64_t dblK = klassOf(p, dbl);
              auto dn = klassName(p, dblK);
              if (dn != "java/lang/Double") continue;
              int dn2 = 0;
              for (auto& df : fields(p, rt, kr, dblK)) {
                if (df.isStatic || df.descriptor != "D") continue;
                if (dn2 == 0) doubleValue_ = (int64_t)df.offset;
                ++dn2;
              }
              if (doubleValue_ >= 0) break;
            }
            spdlog::debug("fov: Double.value offset {}", doubleValue_);
          }
          if (doubleValue_ < 0) {
            spdlog::debug("fov: java/lang/Double.value not resolved");
          } else {
            for (auto& f : optFs) {
              if (f.isStatic || f.descriptor.find("class_7172") == std::string::npos) continue;
              auto oiRaw = p.read<uint32_t>((uintptr_t)(*optObj + (uint64_t)f.offset));
              if (!oiRaw || !*oiRaw) continue;
              uint64_t oi = decodeOop(*oiRaw);
              auto vRaw = p.read<uint32_t>((uintptr_t)(oi + (uint64_t)oiValueOff));
              if (!vRaw || !*vRaw) continue;
              uint64_t dbl = decodeOop(*vRaw);
              auto dblK = klassOf(p, dbl);
              if (klassName(p, dblK) != "java/lang/Double") continue;
              auto dv = p.read<double>((uintptr_t)(dbl + (uint64_t)doubleValue_));
              if (!dv) continue;
              spdlog::debug("fov: Double opt {} @ {} = {:.3f}", f.name, f.offset, *dv);
              if (*dv >= 30.0 && *dv <= 110.0) {
                offFovOpt_ = (int64_t)f.offset;
                spdlog::debug("fov: found fov {} @ {} value={:.1f}", f.name, f.offset, *dv);
                break;
              }
            }
          }
        }
      }
    }
  }
  spdlog::info(
      "model: ready (player off={}, posVec off={}, vec x/y/z={}/{}/{}, rot {}/{}, "
      "level {} gr {} cam {} camPos {} camFwd {} bb {} aabbX {}, lookup {}, "
      "fovOpt {} optVal {} dblV {})",
      offPlayer_, offPositionVec_, vecX_, vecY_, vecZ_, offYaw_, offPitch_, offLevel_,
      offGameRenderer_, offCamera_, offCamPos_, offCamFwd_, offBB_, aabbMinX_,
      offEntitiesLookup_, offFovOpt_, offOptValue_, doubleValue_);

  // ESP discovery (once): level / gameRenderer / options object graphs.
  if (!graphDumped_) {
    graphDumped_ = true;
    auto dumpSub = [&](const char* desc) -> std::optional<uint64_t> {
      for (auto& f : mcFields) {
        if (f.isStatic || f.descriptor != desc) continue;
        auto sub = objectField(p, mcInstance_, f);
        if (sub) return sub;
      }
      return std::nullopt;
    };
    auto findFieldOf = [&](uint64_t obj, const char* desc) -> std::optional<uint64_t> {
      auto kid = p.read<uint32_t>(obj + 8);
      if (!kid) return std::nullopt;
      uint64_t kk = env.compressedKlasses
                        ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                        : (uint64_t)*kid;
      for (auto& f : fields(p, rt, kr, kk)) {
        if (f.isStatic || f.descriptor != desc) continue;
        auto sub = objectField(p, obj, f);
        if (sub) return sub;
      }
      return std::nullopt;
    };

    if (auto level = dumpSub("Lnet/minecraft/class_638;")) {
      dumpObject(p, rt, kr, *level, "level");
      // Level superclass fields (entity storage lives there).
      if (auto kid = p.read<uint32_t>(*level + 8)) {
        uint64_t lk = env.compressedKlasses
                          ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                          : (uint64_t)*kid;
        for (int i = 0; i < 4 && lk; ++i) {
          auto sup = kr.superClass(p, rt, lk);
          if (!sup || !*sup) break;
          lk = *sup;
          auto nmSym = p.read<uint64_t>((uintptr_t)(lk + env.offKlassName));
          auto nm = nmSym && *nmSym ? rt.readSymbol(p, *nmSym).value_or("?") : "?";
          spdlog::debug("graph: super '{}' klass @ {}", nm, util::hex(lk));
          if (nm == "net/minecraft/class_1937") {
            for (auto& f : fields(p, rt, kr, lk)) {
              if (f.isStatic || f.descriptor.find("net/minecraft/class_55") == std::string::npos)
                continue;
              spdlog::debug("graph: level.super field {} {} @ {}", f.descriptor, f.name,
                            f.offset);
              if (auto sub = objectField(p, *level, f))
                dumpObject(p, rt, kr, *sub, "level.super." + f.name);
            }
            break;
          }
        }
      }
      if (auto es = findFieldOf(*level, "Lnet/minecraft/class_5574;")) {
        dumpObject(p, rt, kr, *es, "level.entityStorage");
        // Int2ObjectLinkedOpenHashMap internals: value[] holds the entities.
        auto dumpInt2ObjectMap = [&](uint64_t m) {
          auto kid = p.read<uint32_t>(m + 8);
          if (!kid) return;
          uint64_t mk = env.compressedKlasses
                            ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                            : (uint64_t)*kid;
          int64_t valueOff = -1;
          for (auto& f : fields(p, rt, kr, mk))
            if (f.descriptor == "[Ljava/lang/Object;" && valueOff < 0)
              valueOff = (int64_t)f.offset;
          if (valueOff < 0) return;
          auto arrRaw = p.read<uint32_t>((uintptr_t)(m + valueOff));
          if (!arrRaw || !*arrRaw) return;
          uint64_t arr = oopBase_ + ((uint64_t)*arrRaw << oopShift_);
          auto alen = p.read<uint32_t>(arr + 12);
          if (!alen || *alen > (1u << 22)) return;
          spdlog::debug("graph: entity map @ {} value array len={}", util::hex(m), *alen);
          auto elems = p.readBytes(arr + 16, (size_t)std::min<uint32_t>(*alen, 256) * 4);
          for (size_t i = 0; i + 4 <= elems.size(); ++i) {
            uint32_t raw;
            std::memcpy(&raw, elems.data() + i, 4);
            if (!raw) continue;
            uint64_t eo = oopBase_ + ((uint64_t)raw << oopShift_);
            auto ekid = p.read<uint32_t>(eo + 8);
            if (!ekid) continue;
            uint64_t ek = env.compressedKlasses
                              ? env.narrowKlassBase + ((uint64_t)*ekid << env.narrowKlassShift)
                              : (uint64_t)*ekid;
            auto esym = p.read<uint64_t>((uintptr_t)(ek + env.offKlassName));
            auto en = esym && *esym ? rt.readSymbol(p, *esym).value_or("?") : "?";
            spdlog::debug("graph: entity map[{}] -> {} @ {}", i, en, util::hex(eo));
            if (i > 60) break;
          }
        };
        if (auto esKid = p.read<uint32_t>(*es + 8)) {
          uint64_t esK = env.compressedKlasses
                             ? env.narrowKlassBase + ((uint64_t)*esKid << env.narrowKlassShift)
                             : (uint64_t)*esKid;
          for (auto& f : fields(p, rt, kr, esK)) {
            if (f.isStatic || f.descriptor != "Lit/unimi/dsi/fastutil/ints/Int2ObjectMap;")
              continue;
            if (auto mm = objectField(p, *es, f)) dumpInt2ObjectMap(*mm);
          }
        }
      }
    }
    if (auto gr = dumpSub("Lnet/minecraft/class_757;")) {
      dumpObject(p, rt, kr, *gr, "gameRenderer");
      if (auto camera = findFieldOf(*gr, "Lnet/minecraft/class_4184;"))
        dumpObject(p, rt, kr, *camera, "gameRenderer.camera");
      if (auto lr = findFieldOf(*gr, "Lnet/minecraft/class_898;"))
        dumpObject(p, rt, kr, *lr, "gameRenderer.levelRenderer");
    }
    if (auto opts = dumpSub("Lnet/minecraft/class_315;"))
      dumpObject(p, rt, kr, *opts, "options");
  }
  return true;
}

std::optional<uint64_t> GameModel::objectField(const mem::Process& p, uint64_t obj,
                                               const jvm::FieldDesc& f) {
  if (f.isStatic || f.descriptor.empty() || f.descriptor[0] != 'L') return std::nullopt;
  auto raw = p.read<uint32_t>((uintptr_t)(obj + f.offset));
  if (!raw || *raw == 0) return std::nullopt;
  return decodeOop(*raw);
}

void GameModel::dumpObject(const mem::Process& p, jvm::JvmRuntime& rt, jvm::KlassReader& kr,
                           uint64_t obj, const std::string& label) {
  const JvmEnv& env = rt.env();
  auto kid = p.read<uint32_t>(obj + 8);
  if (!kid) return;
  uint64_t klass = env.compressedKlasses
                       ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                       : (uint64_t)*kid;
  auto nmSym = p.read<uint64_t>((uintptr_t)(klass + env.offKlassName));
  std::string kname = nmSym && *nmSym ? rt.readSymbol(p, *nmSym).value_or("?") : "?";
  spdlog::debug("graph: [{}] obj @ {} klass '{}'", label, util::hex(obj), kname);
  auto& fs = fields(p, rt, kr, klass);
  spdlog::debug("graph: [{}] {} fields", label, fs.size());

  for (auto& f : fs) {
    if (f.isStatic) continue;
    if (f.descriptor.size() > 1 && f.descriptor[0] == 'L') {
      auto sub = objectField(p, obj, f);
      if (!sub) continue;
      auto skid = p.read<uint32_t>(*sub + 8);
      std::string skName = "?";
      if (skid) {
        uint64_t sk = env.compressedKlasses
                          ? env.narrowKlassBase + ((uint64_t)*skid << env.narrowKlassShift)
                          : (uint64_t)*skid;
        auto ssym = p.read<uint64_t>((uintptr_t)(sk + env.offKlassName));
        if (ssym && *ssym) skName = rt.readSymbol(p, *ssym).value_or("?");
      }
      spdlog::debug("graph:   {} {} @ {} -> {} '{}'", f.descriptor, f.name, f.offset,
                    util::hex(*sub), skName);
    } else if (f.descriptor == "D" || f.descriptor == "F") {
      std::string val;
      if (f.descriptor == "D") {
        if (auto v = p.read<double>((uintptr_t)(obj + f.offset)))
          val = std::format("{:.3f}", *v);
      } else {
        if (auto v = p.read<float>((uintptr_t)(obj + f.offset))) val = std::format("{:.3f}", *v);
      }
      spdlog::debug("graph:   {} {} @ {} = {}", f.descriptor, f.name, f.offset, val);
    }
  }
}

// Walks the super chain once per klass and classifies the entity by which
// well-known intermediary classes it inherits (Yarn IDs, stable since 1.14).
int GameModel::classifyEntity(const mem::Process& p, uint64_t klass) {
  auto it = categoryCache_.find(klass);
  if (it != categoryCache_.end()) return it->second;

  const JvmEnv& env = rt_->env();
  auto supOff = rt_->structs().field("Klass", "_super");
  int cat = (int)EntCat::Misc;
  if (supOff) {
    auto reaches = [&](const char* id) {
      uint64_t k = klass;
      for (int d = 0; k && d < 14; ++d) {
        auto nmSym = p.read<uint64_t>((uintptr_t)(k + env.offKlassName));
        if (nmSym && *nmSym) {
          auto nm = rt_->readSymbol(p, *nmSym);
          if (nm && nm->find(id) != std::string::npos) return true;
        }
        auto sup = p.read<uint64_t>((uintptr_t)(k + (uint64_t)*supOff));
        if (!sup || !*sup) break;
        k = *sup;
      }
      return false;
    };
    // Superchain probes cover both runtime naming schemes: intermediary
    // class_XXXX (Fabric/vanilla) and official Mojang paths (Forge/NeoForge).
    auto reachesAny = [&](std::initializer_list<const char*> ids) {
      for (auto* id : ids)
        if (reaches(id)) return true;
      return false;
    };
    if (reachesAny({"class_1308", "/entity/Mob"})) cat = (int)EntCat::Mob;
    else if (reachesAny({"class_1657", "player/Player"})) cat = (int)EntCat::Player;
    else if (reachesAny({"class_1530", "ArmorStand"})) cat = (int)EntCat::ArmorStand;
    else if (reachesAny({"class_1666", "Projectile"})) cat = (int)EntCat::Projectile;
    else if (reachesAny({"class_1665", "ItemEntity"})) cat = (int)EntCat::Item;
    else if (reachesAny({"class_1309", "LivingEntity"})) cat = (int)EntCat::Living;
  }
  return categoryCache_.emplace(klass, cat).first->second;
}

bool GameModel::isEntitySubclass(const mem::Process& p, uint64_t klass) {
  // Walk the super chain looking for the Entity class.
  const JvmEnv& env = rt_->env();
  auto supOff = rt_->structs().field("Klass", "_super");
  if (!supOff) return false;
  uint64_t k = klass;
  for (int d = 0; k && d < 12; ++d) {
    if (k == entityKlass_) return true;
    auto nmSym = p.read<uint64_t>((uintptr_t)(k + env.offKlassName));
    if (nmSym && *nmSym) {
      auto nm = rt_->readSymbol(p, *nmSym);
      if (nm && *nm == "net/minecraft/class_1297") {
        entityKlass_ = k;
        return true;
      }
    }
    auto sup = p.read<uint64_t>((uintptr_t)(k + (uint64_t)*supOff));
    if (!sup || !*sup) break;
    k = *sup;
  }
  return false;
}

bool GameModel::readCamera(const mem::Process& p, CameraSnapshot& out) {
  out.valid = false;
  if (!valid_ || offGameRenderer_ < 0 || offCamera_ < 0 || offCamPos_ < 0 || offCamFwd_ < 0) {
    out.error = "camera offsets not resolved";
    return false;
  }
  auto grRaw = p.read<uint32_t>((uintptr_t)(mcInstance_ + (uint64_t)offGameRenderer_));
  if (!grRaw || !*grRaw) {
    out.error = "no gameRenderer";
    return false;
  }
  uint64_t gr = decodeOop(*grRaw);
  auto camRaw = p.read<uint32_t>((uintptr_t)(gr + (uint64_t)offCamera_));
  if (!camRaw || !*camRaw) {
    out.error = "no camera";
    return false;
  }
  uint64_t cam = decodeOop(*camRaw);

  if (!readVec3(p, cam, offCamPos_, out.pos)) {
    out.error = "camera pos read failed";
    return false;
  }
  auto rdV3f = [&](int64_t off, float v[3]) {
    if (off < 0 || vec3fX_ < 0) return false;
    auto raw = p.read<uint32_t>((uintptr_t)(cam + (uint64_t)off));
    if (!raw || !*raw) return false;
    uint64_t v3 = decodeOop(*raw);
    for (int i = 0; i < 3; ++i) {
      auto fv = p.read<float>(
          (uintptr_t)(v3 + (uint64_t)(vec3fX_ + (uint64_t)i * 4)));
      if (!fv) return false;
      v[i] = *fv;
    }
    return true;
  };
  out.forwards[0] = out.forwards[1] = out.forwards[2] = 0;
  out.up[0] = out.up[1] = out.up[2] = 0;
  out.left[0] = out.left[1] = out.left[2] = 0;
  rdV3f(offCamFwd_, out.forwards);
  rdV3f(offCamUp_, out.up);
  rdV3f(offCamLeft_, out.left);
  out.valid = true;
  return true;
}

// BFS over the oop graph rooted at the ClientLevel object, descending through
// fastutil maps and their Object[] value arrays, collecting Int2ObjectMap
// objects. The entity maps sit at different depths depending on version and
// loader (Fabric 1.21: ClientLevel.entityStorage -> maps; Forge 1.20:
// ClientLevel.entityManager -> PersistentEntitySectionManager ->
// EntitySectionStorage -> Long2ObjectMap<EntitySection> -> EntitySection.storage
// -> EntityLookup.entities), so a fixed field chain cannot be mapped — the
// graph itself is the mapping.
std::vector<uint64_t> GameModel::findEntityMaps(const mem::Process& p, uint64_t level) {
  std::vector<uint64_t> maps;
  if (!valid_ || !level) return maps;
  std::set<uint64_t> seen;
  std::vector<std::pair<uint64_t, int>> queue;
  queue.emplace_back(level, 0);
  seen.insert(level);
  auto pushChild = [&](uint64_t obj, int d) {
    if (!obj || d > 6 || seen.size() > 8192 || queue.size() > 2048) return;
    if (seen.insert(obj).second) queue.emplace_back(obj, d);
  };
  size_t processed = 0;
  while (processed < queue.size() && processed < 1024) {
    auto [obj, d] = queue[processed++];
    uint64_t k = klassOf(p, obj);
    for (auto& f : fields(p, *rt_, *kr_, k)) {
      if (f.isStatic) continue;
      if (f.descriptor.rfind("Lit/unimi/dsi/fastutil/ints/Int2Object", 0) == 0) {
        if (auto raw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)f.offset)); raw && *raw)
          maps.push_back(decodeOop(*raw));
        continue;
      }
      if (f.descriptor.rfind("Lit/unimi/dsi/fastutil/", 0) == 0) {
        if (auto raw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)f.offset)); raw && *raw)
          pushChild(decodeOop(*raw), d + 1);
        continue;
      }
      if (f.descriptor == "[Ljava/lang/Object;") {
        // fastutil map value array: descend into elements (EntitySection...)
        if (auto raw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)f.offset)); raw && *raw) {
          uint64_t arr = decodeOop(*raw);
          auto alen = p.read<uint32_t>(arr + 12);
          if (alen && *alen > 0 && *alen <= (1u << 16)) {
            auto elems = p.readBytes(arr + 16, (size_t)std::min<uint32_t>(*alen, 4096) * 4);
            for (size_t i = 0; i + 4 <= elems.size(); i += 4) {
              uint32_t r2;
              std::memcpy(&r2, elems.data() + i, 4);
              if (r2) pushChild(decodeOop(r2), d + 1);
            }
          }
        }
        continue;
      }
      if (f.descriptor.rfind("Lnet/minecraft/", 0) == 0 && d < 5) {
        // The entity storage path names every hop (EntityManager,
        // EntitySectionStorage, EntitySection, EntityLookup) — descending
        // only into entity-named classes keeps the budget away from huge
        // irrelevant subtrees (LevelRenderer and friends).
        if (auto raw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)f.offset)); raw && *raw) {
          uint64_t child = decodeOop(*raw);
          uint64_t ck = klassOf(p, child);
          auto nmSym = p.read<uint64_t>((uintptr_t)(ck + rt_->env().offKlassName));
          auto nm = nmSym && *nmSym ? rt_->readSymbol(p, *nmSym).value_or(std::string()) : "";
          if (nm.find("ntity") != std::string::npos) pushChild(child, d + 1);
        }
      }
    }
  }
  return maps;
}

bool GameModel::readEntities(const mem::Process& p, std::vector<EspEntity>& out) {
  out.clear();
  if (!valid_ || offLevel_ < 0 || offPositionVec_ < 0 || offBB_ < 0) {
    return false;
  }
  const JvmEnv& env = rt_->env();
  (void)env;

  auto lvRaw = p.read<uint32_t>((uintptr_t)(mcInstance_ + (uint64_t)offLevel_));
  if (!lvRaw || !*lvRaw) return false;
  uint64_t level = decodeOop(*lvRaw);

  // The entity maps hold Entity oops in their value arrays. Iterate all found
  // maps, dedupe by address.
  auto maps = findEntityMaps(p, level);
  static std::chrono::steady_clock::time_point lastLog{};
  if (maps.empty()) {
    if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(2)) {
      lastLog = std::chrono::steady_clock::now();
      spdlog::debug("entities: no Int2Object maps reachable from level (BFS)");
    }
    return false;
  }
  if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(2)) {
    lastLog = std::chrono::steady_clock::now();
    spdlog::debug("entities: {} candidate maps from BFS", maps.size());
  }

  // offsets of the value[] field per map class — fastutil map classes have
  // different layouts (OpenHashMap / LinkedOpenHashMap / ArrayMap), so the
  // offset is resolved per klass and cached.
  std::map<uint64_t, int64_t> valueOffByKlass;
  auto valueOffFor = [&](uint64_t m) -> int64_t {
    uint64_t mk = klassOf(p, m);
    auto it = valueOffByKlass.find(mk);
    if (it != valueOffByKlass.end()) return it->second;
    int64_t off = -1;
    for (auto& f : fields(p, *rt_, *kr_, mk))
      if (f.descriptor == "[Ljava/lang/Object;") {
        off = (int64_t)f.offset;
        break;
      }
    valueOffByKlass.emplace(mk, off);
    return off;
  };

  std::set<uint64_t> seen;
  uint64_t localPlayer = 0;
  if (auto pRaw = p.read<uint32_t>((uintptr_t)(mcInstance_ + (uint64_t)offPlayer_)); pRaw && *pRaw)
    localPlayer = decodeOop(*pRaw);
  size_t statElems = 0, statNonNull = 0, statNamed = 0, statPos = 0, statBB = 0;
  for (size_t mi = 0; mi < maps.size(); ++mi) {
    uint64_t m = maps[mi];
    int64_t valueOff = valueOffFor(m);
    if (valueOff < 0) continue;
    auto arrRaw = p.read<uint32_t>((uintptr_t)(m + (uint64_t)valueOff));
    if (!arrRaw || !*arrRaw) continue;
    uint64_t arr = decodeOop(*arrRaw);
    auto alen = p.read<uint32_t>(arr + 12);
    if (!alen || *alen > (1u << 22)) continue;
    auto elems = p.readBytes(arr + 16, (size_t)*alen * 4);
    for (size_t i = 0; i + 4 <= elems.size(); ++i) {
      uint32_t raw;
      std::memcpy(&raw, elems.data() + i, 4);
      ++statElems;
      if (!raw) continue;
      ++statNonNull;
      uint64_t obj = decodeOop(raw);
      if (!seen.insert(obj).second) continue;
      auto kid = p.read<uint32_t>(obj + 8);
      if (!kid) continue;
      uint64_t kl = env.compressedKlasses
                        ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                        : (uint64_t)*kid;
      EspEntity e;
      e.obj = obj;
      e.className = klassName(p, kl);
      if (e.className.rfind("net/minecraft/", 0) != 0) continue;
      ++statNamed;
      e.category = classifyEntity(p, kl);
      e.self = (obj == localPlayer);
      // current position (Vec3) + previous tick position (xo/yo/zo)
      if (auto pvRaw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)offPositionVec_));
          pvRaw && *pvRaw) {
        uint64_t vec = decodeOop(*pvRaw);
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
          auto v = p.read<double>((uintptr_t)(vec + (uint64_t)(vecX_ + (uint64_t)c * 8)));
          if (!v) {
            ok = false;
            break;
          }
          e.cur[c] = *v;
        }
        if (!ok) continue;
        ++statPos;
      } else {
        continue;
      }

      // Tick interpolation state: prev = our previous-frame cur — exactly the
      // game's xo/yo/zo semantics, independent of the xo field heuristic. The
      // phase (tickPartial) is estimated from the latest change across all
      // entities (world ticks are synchronized), so it stays correct even
      // when the local player stands still.
      auto nowTick = std::chrono::steady_clock::now();
      auto& tk = entTick_[obj];
      if (!tk.has) {
        tk.has = true;
        tk.cur[0] = tk.prev[0] = e.cur[0];
        tk.cur[1] = tk.prev[1] = e.cur[1];
        tk.cur[2] = tk.prev[2] = e.cur[2];
      } else if (tk.cur[0] != e.cur[0] || tk.cur[1] != e.cur[1] || tk.cur[2] != e.cur[2]) {
        tk.prev[0] = tk.cur[0];
        tk.prev[1] = tk.cur[1];
        tk.prev[2] = tk.cur[2];
        tk.cur[0] = e.cur[0];
        tk.cur[1] = e.cur[1];
        tk.cur[2] = e.cur[2];
        lastTickChange_ = nowTick;
      }
      e.prev[0] = tk.prev[0];
      e.prev[1] = tk.prev[1];
      e.prev[2] = tk.prev[2];
      if (entTick_.size() > 4096) {
        // prune entities that left the world (not seen this pass)
        std::set<uint64_t> alive;
        for (auto& pe : out) alive.insert(pe.obj);
        alive.insert(obj);
        for (auto it = entTick_.begin(); it != entTick_.end();) {
          if (!alive.count(it->first))
            it = entTick_.erase(it);
          else
            ++it;
        }
      }
      bool pok = true;
      for (int c = 0; c < 3; ++c) {
        int64_t off = c == 0 ? prevX_ : c == 1 ? prevY_ : prevZ_;
        if (off < 0) {
          pok = false;
          break;
        }
        auto v = p.read<double>((uintptr_t)(obj + (uint64_t)off));
        if (!v) {
          pok = false;
          break;
        }
        e.prev[c] = *v;
      }
      if (!pok) e.prev[0] = e.cur[0], e.prev[1] = e.cur[1], e.prev[2] = e.cur[2];
      // bounding box
      auto bbRaw = p.read<uint32_t>((uintptr_t)(obj + (uint64_t)offBB_));
      if (!bbRaw || !*bbRaw) continue;
      uint64_t bb = decodeOop(*bbRaw);
      bool ok = true;
      for (int c = 0; c < 6; ++c) {
        auto v = p.read<double>(
            (uintptr_t)(bb + (uint64_t)(aabbMinX_ + (uint64_t)c * 8)));
        if (!v) {
          ok = false;
          break;
        }
        double d = *v;
        if (std::isnan(d) || std::abs(d) > 1e7) {
          ok = false;
          break;
        }
        (c < 3 ? e.bbMin[c] : e.bbMax[c - 3]) = d;
      }
      if (!ok) continue;
      ++statBB;
      out.push_back(std::move(e));
      if (out.size() >= 256) return true;
    }
  }
  static std::chrono::steady_clock::time_point lastParsedLog{};
  if (std::chrono::steady_clock::now() - lastParsedLog > std::chrono::seconds(2)) {
    lastParsedLog = std::chrono::steady_clock::now();
    spdlog::debug("entities: {} parsed from {} maps (elems={} nn={} named={} pos={} bb={})",
                  out.size(), maps.size(), statElems, statNonNull, statNamed, statPos, statBB);
  }
  return !out.empty();
}

double GameModel::tickPartial() const {
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastTickChange_)
                 .count() /
             0.05;
  return std::clamp(t, 0.0, 1.0);
}

float GameModel::readFovFromFile(const mem::Process& p) {
  // MC stores fov as normalized 0-1 double in options.txt; degrees = 70+40*v.
  // The game directory = MC process current working directory (from PEB).
  namespace fs = std::filesystem;
  std::string gameDir;
  {
    typedef LONG NTSTATUS;
    typedef struct _PBI {
      void* r1;
      void* PebBaseAddress;
      void* r2[2];
      ULONG_PTR pid;
      void* r3;
    } PBI;
    using NtQIP_t = LONG(WINAPI*)(HANDLE, ULONG, void*, ULONG, void*);
    auto NT_SUCCESS = [](LONG s) { return s >= 0; };
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto nqip = ntdll ? (NtQIP_t)GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr;
    if (nqip && p.valid()) {
      PBI pbi = {};
      if (nqip(p.handle(), 0, &pbi, sizeof(pbi), nullptr) >= 0 && pbi.PebBaseAddress) {
        auto pp = p.read<uint64_t>((uintptr_t)((uintptr_t)pbi.PebBaseAddress + 0x20));
        if (pp && *pp) {
          auto usLen = p.read<uint16_t>((uintptr_t)(*pp + 0x38));
          auto usBuf = p.read<uint64_t>((uintptr_t)(*pp + 0x40));
          if (usLen && usBuf && *usLen > 0 && *usLen < 2048) {
            auto wbuf = p.readBytes(*usBuf, *usLen);
            std::wstring cwd((const wchar_t*)wbuf.data(), wbuf.size() / 2);
            gameDir = util::toUtf8(cwd);
            spdlog::debug("fov: MC cwd = {}", gameDir);
          }
        }
      }
    }
  }
  if (gameDir.empty()) return 70.f;
  fs::path opts = fs::path(gameDir) / "options.txt";
  std::ifstream f(opts);
  if (!f) return 70.f;
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("fov:", 0) != 0) continue;
    try {
      double v = std::stod(line.substr(4));
      if (v >= 0.0 && v <= 1.0) {
        float deg = (float)(70.0 + 40.0 * v);
        spdlog::info("fov: from {} value={:.1f} -> {} deg", opts.string(), v, deg);
        return deg;
      }
    } catch (...) {
    }
    break;
  }
  return 70.f;
}

float GameModel::readFov(const mem::Process& p) {
  // default vertical FOV in degrees; overridden by the options chain below
  float fov = 70.f;
  if (offOptions_ < 0 || offFovOpt_ < 0 || offOptValue_ < 0) {
    spdlog::debug("fov: chain unresolved (opts={} fovOpt={} optValue={})", offOptions_,
                  offFovOpt_, offOptValue_);
    return fov;
  }
  auto optRaw = p.read<uint32_t>((uintptr_t)(mcInstance_ + (uint64_t)offOptions_));
  if (!optRaw || !*optRaw) {
    spdlog::debug("fov: options object null");
    return fov;
  }
  uint64_t opts = decodeOop(*optRaw);
  auto oiRaw = p.read<uint32_t>((uintptr_t)(opts + (uint64_t)offFovOpt_));
  if (!oiRaw || !*oiRaw) {
    spdlog::debug("fov: fov OptionInstance null");
    return fov;
  }
  uint64_t oi = decodeOop(*oiRaw);
  auto valRaw = p.read<uint32_t>((uintptr_t)(oi + (uint64_t)offOptValue_));
  if (!valRaw || !*valRaw) {
    spdlog::debug("fov: value null");
    return fov;
  }
  uint64_t dbl = decodeOop(*valRaw);
  auto dv = p.read<double>((uintptr_t)(dbl + (uint64_t)doubleValue_));
  if (!dv) {
    spdlog::debug("fov: double unreadable");
    return fov;
  }
  // MC stores the fov slider as normalized 0..1 (0=Normal/70°, 1=QuakePro/110°)
  // in some versions and as raw degrees (30..110) in others.
  if (*dv >= 0.0 && *dv <= 1.0)
    fov = (float)(70.0 + 40.0 * *dv);
  else if (*dv >= 20.0 && *dv <= 150.0)
    fov = (float)*dv;
  spdlog::debug("fov: raw={:.3f} -> {} deg", *dv, fov);
  return fov;
}

bool GameModel::readPlayer(const mem::Process& p, EntitySnapshot& out) {
  out.valid = false;
  if (!valid_ || offPlayer_ < 0 || offPositionVec_ < 0) {
    out.error = "model not ready";
    return false;
  }
  auto raw = p.read<uint32_t>((uintptr_t)(mcInstance_ + offPlayer_));
  if (!raw || *raw == 0) {
    out.error = "no player";
    return false;
  }
  uint64_t player = decodeOop(*raw);
  out.playerObj = player;

  auto vecRaw = p.read<uint32_t>((uintptr_t)(player + (uint64_t)offPositionVec_));
  if (!vecRaw || *vecRaw == 0) {
    out.error = "no position vec";
    return false;
  }
  uint64_t vec = decodeOop(*vecRaw);

  auto rdD = [&](int64_t off, double& v) {
    auto r = p.read<double>((uintptr_t)(vec + (uint64_t)off));
    if (!r) return false;
    v = *r;
    return !std::isnan(v) && std::abs(v) < 1e7;
  };
  double x = 0, y = 0, z = 0;
  if (!rdD(vecX_, x) || !rdD(vecY_, y) || !rdD(vecZ_, z)) {
    out.error = "position read failed";
    return false;
  }
  out.x = x;
  out.y = y;
  out.z = z;
  if (offYaw_ >= 0) {
    if (auto v = p.read<float>((uintptr_t)(player + (uint64_t)offYaw_))) out.yaw = *v;
  }
  if (offPitch_ >= 0) {
    if (auto v = p.read<float>((uintptr_t)(player + (uint64_t)offPitch_))) out.pitch = *v;
  }
  out.valid = true;
  return true;
}

} // namespace mc
