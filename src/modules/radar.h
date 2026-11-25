#pragma once
#include "modules/module.h"

namespace mods {

class RadarModule : public Module {
public:
  RadarModule();
  void update(const ModuleContext& ctx) override;
};

} // namespace mods