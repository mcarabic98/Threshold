#pragma once
#include "modules/module.h"

namespace mods {

// ESP: entity AABBs projected to the overlay, colored by classification.
// Interpolation state lives in GameModel (per-entity tick cache); the module
// only renders.
class EspModule : public Module {
public:
  EspModule();
  void update(const ModuleContext& ctx) override;
};

} // namespace mods
