#pragma once
#include "modules/module.h"

namespace mods {

class TracersModule : public Module {
public:
  TracersModule();
  void update(const ModuleContext& ctx) override;
};

} // namespace mods