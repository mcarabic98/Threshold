#pragma once
#include "modules/module.h"

namespace mods {

// ESP: entity AABBs projected to the overlay, colored by classification.
// The former App::drawEsp, unchanged; the menu filters became module settings.
class EspModule : public Module {
public:
  EspModule();
  void update(const ModuleContext& ctx) override;

private:
  // game FOV cache: re-read every ~5s while not overridden manually
  std::chrono::steady_clock::time_point lastFovRead_{};
  float cachedFov_ = 70.f;
  // tick-phase estimation: the player position change pins the 20 tps phase
  std::chrono::steady_clock::time_point lastPosChange_{};
  mc::EntitySnapshot lastPlayer_{};
  bool hasLastPlayer_ = false;
};

} // namespace mods
