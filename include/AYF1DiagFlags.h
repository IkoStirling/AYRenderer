#pragma once
// F1 SIGSEGV diagnosis flags — docs/f1-sigsegv-repro.md
//
// Toggle via CMake (must match on AYRenderer lib AND AYRenderer_Test):
//   -DAY_F1_DIAG_LIGHT=ON/OFF
//   -DAY_F1_DIAG_FRAME_SHADOW=ON/OFF
//
// AY_F1_DIAG_DEFAULT_SHADOW was retired in E4 (§5.4, 2026-07-22):
// the canonical default pipeline now mounts Shadow disabled
// uniformly, so the compile-time toggle between "5-pass shadow
// enabled" and "4-pass no-shadow" no longer exists — both states
// are reachable at runtime via findPass("Shadow")->setEnabled(...).
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

#include <cstddef>

namespace ayt::render
{
// Defined in AYRenderer.cpp (has FrameContext). Used by RendererSubSystem
// diag without including FrameContext.h into the SubSystem TU.
std::size_t detailDiagSizeofFrameContext();
} // namespace ayt::render
