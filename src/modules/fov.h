#pragma once
#include "modules/module.h"

namespace mods {

// Centralized FOV provider. Reads FOV from game (options file/heap chain)
// with caching, supports manual override, and exposes the effective FOV
// for other modules (ESP, tracers, aim, etc.) so they don't each re-read it.
// Also tracks zoom state if a zoom module is active.
class FovModule : public Module {
public:
  FovModule();
  void update(const ModuleContext& ctx) override;

  // Effective vertical FOV in degrees for projection.
  float fovDeg() const { return effectiveFov_; }
  // Whether manual override is active.
  bool manual() const { return boolVal("fovManual"); }

  // Static accessor: finds the FOV module in the registry.
  static FovModule* get(ModuleRegistry& reg);

private:
  float effectiveFov_ = 70.f;
  float cachedFov_ = 70.f;
  std::chrono::steady_clock::time_point lastFovRead_{};
};

} // namespace mods
