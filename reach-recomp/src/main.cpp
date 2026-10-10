// reach - ReXGlue Recompiled Project

#include "generated/default/reach_init.h"

#include "reach_app.h"

#include <rex/cvar.h>

REX_DEFINE_APP(reach, ReachApp::Create)

REXCVAR_DEFINE_STRING(gpu_backend, "vulkan", "GPU",
                      "GPU backend on Windows: vulkan (with this project's fixes) or d3d12")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::string ReachGpuBackend() { return REXCVAR_GET(gpu_backend); }
