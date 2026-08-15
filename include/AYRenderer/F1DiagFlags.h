#pragma once
// F1 SIGSEGV diagnosis flags — docs/f1-sigsegv-repro.md
//
// §5.5 cleanup (2026-07-22) — the two F1-diagnostic compile flags
// (AY_F1_DIAG_LIGHT and AY_F1_DIAG_FRAME_SHADOW) are now RETIRED.
// They were the §5.5 PR-F1' C' forbidden-combo toggle switches
// (Light struct + FrameContext shadow writeback combined with
// default-on Shadow). E5 ships default-on Shadow WITHOUT the
// forbidden combos, so the flags have nothing left to gate and
// are removed. AY_F1_DIAG_DEFAULT_SHADOW was already retired in E4.
//
// The header is kept as a thin include-only marker for one frame
// (some legacy TUs include it transitively via AYRenderer/RenderScene.h /
// AYRenderer.h). It defines both macros to 0 unconditionally so
// any stale `#if AY_F1_DIAG_LIGHT` / `#if AY_F1_DIAG_FRAME_SHADOW`
// in a TU that hasn't been edited yet still compiles with the
// "diagnostic off" branch selected. The CMake options are also
// removed; the corresponding target_compile_definitions lines
// stop being emitted so the macros become pure header defaults.

#define AY_F1_DIAG_LIGHT 0
#define AY_F1_DIAG_FRAME_SHADOW 0

#include <cstddef>

namespace ayt::render
{
// Defined in AYRenderer.cpp (has FrameContext). Used by RendererSubSystem
// diag without including FrameContext.h into the SubSystem TU.
// Retained — the ABI guard for sizeof(FrameContext) is still useful
// even after the diagnostic fields are gone (a future ABI bump on
// this struct is the most likely cause of the EventBus _Orphan_all
// crash originally attributed to the F1 diag toggles).
std::size_t detailDiagSizeofFrameContext();
} // namespace ayt::render
