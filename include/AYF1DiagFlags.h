#pragma once
// F1 SIGSEGV diagnosis flags — docs/f1-sigsegv-repro.md
//
// Toggle via CMake (must match on AYRenderer lib AND AYRenderer_Test):
//   -DAY_F1_DIAG_LIGHT=ON/OFF
//   -DAY_F1_DIAG_FRAME_SHADOW=ON/OFF
//   -DAY_F1_DIAG_DEFAULT_SHADOW=ON/OFF
//
// After ANY flag change: Clean rebuild both targets. Incremental builds
// with mismatched sizeof(RendererSubSystem) reproduce the EventBus
// _Orphan_all crash (0xFFFFFFFFFFFFFFFF) without any "real" EventBus bug.

#ifndef AY_F1_DIAG_LIGHT
#  define AY_F1_DIAG_LIGHT 0
#endif
#ifndef AY_F1_DIAG_FRAME_SHADOW
#  define AY_F1_DIAG_FRAME_SHADOW 0
#endif
#ifndef AY_F1_DIAG_DEFAULT_SHADOW
#  define AY_F1_DIAG_DEFAULT_SHADOW 0
#endif

#include <cstddef>

namespace ayt::render
{
// Defined in AYRenderer.cpp (has FrameContext). Used by RendererSubSystem
// diag without including FrameContext.h into the SubSystem TU.
std::size_t detailDiagSizeofFrameContext();
} // namespace ayt::render
