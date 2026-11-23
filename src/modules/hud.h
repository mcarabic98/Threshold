#pragma once
#include "modules/module.h"

namespace mods {

// HUD tiles fed with live data from the context; neither reads the game
// itself beyond what App already put into ModuleContext.
class FpsModule : public Module {
public:
  FpsModule();
  void update(const ModuleContext& ctx) override;
};

class CoordsModule : public Module {
public:
  CoordsModule();
  void update(const ModuleContext& ctx) override;
};

} // namespace mods
