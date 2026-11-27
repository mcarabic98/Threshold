#pragma once
#include <filesystem>
#include <set>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// One field rule: which class owns it, intermediary name, descriptor.
struct FieldRule {
  std::string owner; // class key from MappingSet::classes
  std::string name;  // intermediary field name (field_XXXXX)
  std::string type;  // descriptor ("Lnet/minecraft/class_746;" / "F" / ...)
};

// Per-version table of the classes/fields the client navigates. Shipped as
// mappings/<version>.json next to the exe; unknown versions fall back to
// default.json, then to the built-in 1.21.1 table.
struct MappingSet {
  bool ok = false;
  bool builtin = true;
  std::string version = "unknown";
  std::map<std::string, std::string> classes;  // key -> intermediary class name
  std::map<std::string, FieldRule> fields;     // key -> rule

  std::optional<std::string> className(const std::string& key) const;
  const FieldRule* field(const std::string& key) const;
  std::vector<std::string> targetClasses() const; // entity classes for the heap scan

  // keys whose classes are heap-scan targets (entity types we want to find)
  static const std::set<std::string>& targetKeys() {
    static const std::set<std::string> keys = {"minecraft", "localPlayer", "entity", "vec3",
                                               "aabb"};
    return keys;
  }
};

// Pull the version out of a window title ("Minecraft* 1.21.1 - ...").
std::string versionFromTitle(const std::wstring& title);

// Loader hint from the window title: "Minecraft* Forge 1.20.1" -> "forge".
// Empty when the title does not name a loader (vanilla/Fabric).
std::string loaderFromTitle(const std::wstring& title);

MappingSet loadMappings(const std::filesystem::path& dir, const std::string& version,
                        const std::string& loader = {});

} // namespace mc
