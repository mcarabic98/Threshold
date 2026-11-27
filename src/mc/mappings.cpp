#include "mc/mappings.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <set>

namespace mc {

namespace {

// Built-in fallback: Fabric intermediary IDs for 1.21.1. Verified live.
MappingSet builtin1211() {
  MappingSet m;
  m.version = "1.21.1";
  m.classes = {
      {"minecraft", "net/minecraft/class_310"},
      {"localPlayer", "net/minecraft/class_746"},
      {"abstractClientPlayer", "net/minecraft/class_742"},
      {"player", "net/minecraft/class_1657"},
      {"livingEntity", "net/minecraft/class_1309"},
      {"entity", "net/minecraft/class_1297"},
      {"clientLevel", "net/minecraft/class_638"},
      {"vec3", "net/minecraft/class_243"},
  };
  m.fields = {
      {"player", {"minecraft", "field_1724", "Lnet/minecraft/class_746;"}},
      {"position", {"entity", "field_22467", "Lnet/minecraft/class_243;"}},
      {"yRot", {"entity", "field_6031", "F"}},
      {"xRot", {"entity", "field_5965", "F"}},
  };
  m.ok = true;
  return m;
}

bool fromJson(MappingSet& m, const nlohmann::json& j) {
  if (auto v = j.find("version"); v != j.end() && v->is_string()) m.version = v->get<std::string>();
  if (auto v = j.find("classes"); v != j.end() && v->is_object())
    for (auto& [k, val] : v->items())
      if (val.is_string()) m.classes[k] = val.get<std::string>();
  if (auto v = j.find("fields"); v != j.end() && v->is_object()) {
    for (auto& [k, val] : v->items()) {
      if (!val.is_object()) continue;
      FieldRule fr;
      if (auto o = val.find("owner"); o != val.end() && o->is_string())
        fr.owner = o->get<std::string>();
      if (auto o = val.find("name"); o != val.end() && o->is_string())
        fr.name = o->get<std::string>();
      if (auto o = val.find("type"); o != val.end() && o->is_string())
        fr.type = o->get<std::string>();
      if (!fr.name.empty() && !fr.type.empty()) m.fields[k] = std::move(fr);
    }
  }
  return !m.classes.empty() && !m.fields.empty();
}

} // namespace

std::optional<std::string> MappingSet::className(const std::string& key) const {
  auto it = classes.find(key);
  if (it == classes.end()) return std::nullopt;
  return it->second;
}

const FieldRule* MappingSet::field(const std::string& key) const {
  auto it = fields.find(key);
  if (it == fields.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> MappingSet::targetClasses() const {
  // Only entity-ish classes are heap-scan targets; camera/render helpers are
  // reached through navigation, not the scan.
  std::set<std::string> s;
  for (auto& [key, name] : classes)
    if (targetKeys().count(key)) s.insert(name);
  return {s.begin(), s.end()};
}

std::string versionFromTitle(const std::wstring& title) {
  // "Minecraft* 1.21.1 - ..." -> "1.21.1"; first digit-led x.y[.z] group.
  std::string t;
  t.reserve(title.size());
  for (wchar_t c : title) {
    if (c < 128) t += (char)c;
  }
  for (size_t i = 0; i + 2 < t.size(); ++i) {
    if (!std::isdigit((unsigned char)t[i])) continue;
    size_t j = i;
    size_t dots = 0;
    while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == '.')) {
      if (t[j] == '.') ++dots;
      ++j;
    }
    if (dots >= 1 && j > i + 1) return t.substr(i, j - i);
    i = j;
  }
  return "unknown";
}

std::string loaderFromTitle(const std::wstring& title) {
  std::string t;
  t.reserve(title.size());
  for (wchar_t c : title) {
    if (c < 128) t += (char)(char)std::tolower((unsigned char)c);
  }
  if (t.find("neoforge") != std::string::npos) return "neoforge";
  if (t.find("forge") != std::string::npos) return "forge";
  if (t.find("fabric") != std::string::npos) return "fabric";
  return {};
}

// Runtime naming per loader: Fabric/vanilla ship intermediary class_XXXX
// names, Forge/NeoForge (1.17+) ship official Mojang class names with SRG
// field names. Class names are stable across versions within a scheme; SRG
// field names confirmed live on Forge 1.20.1 (JDK 17) — fields whose SRG id
// is not pinned here resolve by descriptor type instead (see GameModel).
MappingSet builtinOfficial() {
  MappingSet m;
  m.version = "official";
  m.classes = {
      {"minecraft", "net/minecraft/client/Minecraft"},
      {"localPlayer", "net/minecraft/client/player/LocalPlayer"},
      {"abstractClientPlayer", "net/minecraft/client/player/AbstractClientPlayer"},
      {"player", "net/minecraft/world/entity/player/Player"},
      {"livingEntity", "net/minecraft/world/entity/LivingEntity"},
      {"entity", "net/minecraft/world/entity/Entity"},
      {"clientLevel", "net/minecraft/client/multiplayer/ClientLevel"},
      {"vec3", "net/minecraft/world/phys/Vec3"},
      {"aabb", "net/minecraft/world/phys/AABB"},
      {"gameRenderer", "net/minecraft/client/renderer/GameRenderer"},
      {"camera", "net/minecraft/client/Camera"},
      {"entityStorage", "net/minecraft/world/level/entity/EntitySectionStorage"},
  };
  m.fields = {
      {"player", {"minecraft", "f_91074_", "Lnet/minecraft/client/player/LocalPlayer;"}},
      {"level", {"minecraft", "f_91073_", "Lnet/minecraft/client/multiplayer/ClientLevel;"}},
      {"gameRenderer",
       {"minecraft", "f_91063_", "Lnet/minecraft/client/renderer/GameRenderer;"}},
      {"camera", {"gameRenderer", "", "Lnet/minecraft/client/Camera;"}},
      {"position", {"entity", "", "Lnet/minecraft/world/phys/Vec3;"}},
      {"yRot", {"entity", "", "F"}},
      {"xRot", {"entity", "", "F"}},
  };
  m.ok = true;
  return m;
}

MappingSet loadMappings(const std::filesystem::path& dir, const std::string& version,
                        const std::string& loader) {
  std::vector<std::string> names;
  if (!loader.empty()) names.push_back(version + "-" + loader + ".json");
  names.push_back(version + ".json");
  // default.json ships intermediary names — it must not hijack Forge/NeoForge
  // sessions whose runtime classes are Mojang-mapped.
  if (loader != "forge" && loader != "neoforge") names.push_back("default.json");
  for (auto& name : names) {
    std::filesystem::path p = dir / name;
    std::ifstream f(p);
    if (!f) continue;
    try {
      MappingSet m;
      m.builtin = false;
      if (fromJson(m, nlohmann::json::parse(f))) {
        if (m.version == "unknown") m.version = version;
        spdlog::info("mappings: version '{}' loaded from {}", m.version, p.string());
        return m;
      }
      spdlog::warn("mappings: {} parsed but empty", p.string());
    } catch (const std::exception& e) {
      spdlog::warn("mappings: {} parse failed: {}", p.string(), e.what());
    }
  }
  // Built-in fallback keyed by the runtime naming scheme: Forge/NeoForge run
  // Mojang-mapped classes, everything else runs intermediary.
  MappingSet m;
  if (loader == "forge" || loader == "neoforge") {
    m = builtinOfficial();
    spdlog::info("mappings: using built-in official (Forge/NeoForge) names for '{}'", version);
  } else {
    m = builtin1211();
    if (version != "unknown" && version != m.version)
      spdlog::warn("mappings: no table for version '{}', using built-in '{}' IDs "
                   "(field offsets may mismatch)",
                   version, m.version);
  }
  return m;
}

} // namespace mc
