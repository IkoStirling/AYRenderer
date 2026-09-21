# TAA history rejection investigation — 2026-09-21

Status: **cause reproduced; v10 production fix implemented and GPU-verified;
user-camera visual acceptance pending**.

## v10 resolution

The original investigation below describes v9. Production now preserves
`clamp(historyWeight, 0, 1)` as history confidence and multiplies the final
feedback by it. RGB normalization remains separate. The epsilon guard and all
depth rejection rules are unchanged; the experimental 0.5 hard gate was not
adopted. Full valid support retains the previous feedback; partial support
reduces it continuously rather than discontinuously at a new threshold.

D3D11/D3D12 support regression: 682 checks each, covering positive/negative
subpixel motion, tiny surviving support, values around 0.5, RGBA32F/RGBA16F,
full-support real motion and depth disocclusion. The default 127x127 integrated
probe passes 1004 checks per backend: near-cube rejection steps fall from
0.425537 to 0.0273438; all six scenes have zero steps above 0.05 on rejected
pixels. D3D11 32x32 and 128x128 also show no such steps. Full Renderer: 4486
checks. GPU resident history and existing edge/motion regressions still pass.

This is a confidence fix, not a claim of zero normal temporal ripple. It adds
no RT, draw or sample. Cache key: `taa_phoskia_supported_history_v10`.

## Question and scope

During the original investigation, the user accepted small temporal ripple, but not abrupt dots/protrusions at a
stationary silhouette. This investigation isolates history rejection from the
ordinary 8-phase/0.92-feedback ripple. Editor settings, production shaders,
feedback, depth tolerance and installed binaries are unchanged.

## Confirmed mechanism

`TAAPass.cpp` validates four history taps against the selected surface, zeros
incompatible tap weights, and accepts history when the surviving weight exceeds
`0.00001`. It divides the RGB sum by that surviving weight, but does **not** use
the surviving weight to reduce temporal feedback. A near-zero contribution can
thus be promoted to an entire history sample, then receive 0.92 feedback.

Nearest-surface dilation can change the history depth tag near a silhouette.
When the dominant tap fails validation, tiny bilinear neighbor weights caused
by small UV/velocity residuals can remain above the gate. Normalizing these taps
pulls a neighbor's foreground color outside the silhouette. A subsequent phase
selects background, rejects this history and snaps back. This is **spurious
acceptance followed by rejection**, not evidence that all rejection should be
weakened. The v9 slope tolerance does not address this loss of support/confidence.

GPU trace, D3D11, 127x127 near cube over blue background, pixel (90,108):

| Render frame | Current red | Surviving history weight | Reconstructed history red | Feedback | Consequence |
|---|---:|---:|---:|---:|---|
| 9 | 0 | 0.0000151992 | 0.140503 | 0.919922 | Output red becomes 0.12915 |
| 10 | 0 | 0 | 0 | 0 | Output snaps back to 0 |

The frame-10 depth diagnostic changes from the previous foreground history tag
0.838867 to background depth 0.988281. Motion coverage is present. The raw tap
depths are foreground; the history rejection itself is consistent with those
tags, but the preceding color contamination should not have been amplified.

## Integration probe and A/B evidence

`AYRenderer_TAARejectionGpu` uses the production Phoskia GBuffer shader and real
MRT/D24S8, followed by the production Motion shader with borrowed depth and
LEQUAL, then the production Resolve source. A test-only copy adds rejection
reasons and intermediate outputs. Scene color is a linear-filtered RGBA8 copy
of albedo (not complete lighting/postprocessing); Motion/history use RGBA16F.

Scenes: planar slanted edge, rotated near cube, cube crossing the near clip
plane; each against clear background and an opaque distant plane. Each runs
40 jittered frames; reset-step statistics exclude the first eight frames.

32x32 initially showed no missing Motion and no rejection-associated color
steps above 0.05. 127x127 exposed the fault; 128x128 also reproduced it, so this
is **not exclusively an odd-resolution bug**. Different raster coverage and
small sampling residuals affect how often the unsafe normalization occurs.

The counterfactual changes ONLY the valid-history support gate, from 0.00001
to 0.5, in a test-only source copy. This is an isolation experiment, **not a
chosen production threshold or an approved algorithm change**.

At 127x127, maximum absolute red-channel change on rejected pixels (0–1 units):

| Scene over opaque background | D3D11 production | D3D12 production | Test-only guard, both backends |
|---|---:|---:|---:|
| Slanted plane | 0.544922 | 0.597656 | 0.00489426 |
| Near cube | 0.425537 | 0.425537 | 0.0273438 |
| Near-clipped cube | 0.0582886 | 0.0582886 | approximately 0.000210 |

Across all six guarded scenes, no rejection-associated step exceeds 0.05.
Ordinary accepted-history ripple remains. All covered geometry pixels had valid
Motion in these probes; that candidate was not reproduced. These measurements
are not direct captures of the user's editor camera or a perceptual threshold.

## Reproduction

Run each suite in a fresh process, not after the full Noop test suite:

```powershell
$env:AY_TAA_GPU_TEST = 'd3d11' # Repeat with d3d12.
$env:AY_TAA_PROBE_SIZE = '127' # Also inspected 32 and 128.
Remove-Item Env:AY_TAA_PROBE_LEGACY_FEEDBACK -ErrorAction SilentlyContinue
& .\out\build\windows-debug-vs2026-insider\AYRuntime\AYRenderer\unittest\AYRenderer_Test.exe AYRenderer_TAARejectionGpu
# Test-only v9 control: remove the confidence multiplier to reproduce the bug.
$env:AY_TAA_PROBE_LEGACY_FEEDBACK = '1'
& .\out\build\windows-debug-vs2026-insider\AYRuntime\AYRenderer\unittest\AYRenderer_Test.exe AYRenderer_TAARejectionGpu
```

Output `reasons` bins: accepted, no history, invalid Motion, extreme footprint,
previous depth outside clip range, no compatible history support, other gate
(not a production 0.5 support guard). Watched-pixel detail mode 7 reports
surface UV / reconstructed history red / current red; mode 9 reports history
fraction / depth tolerance / surviving support. Detail output is RGBA16F.

The default tests now assert the corrected behavior. Only the explicit legacy
control asserts the known baseline failure at 127x127.

## Original fix constraints and remaining acceptance

The implemented fix preserves surviving tap support as history confidence instead of converting any
nonzero support into full confidence. Verify tiny-support handling and gradual
feedback separately from depth rejection; do not globally relax depth checks.
True subpixel motion, disocclusion and the existing thin-geometry regression
are verified. No hard support threshold was adopted. User-camera verification
remains necessary after implementation.

The probe reads back between stages and uploads recursive history. It does not
replace the separate GPU-resident ping-pong test or Editor end-to-end capture.
It also does not test transparent layering, complete lighting, cutout/skinned
depth replay, or all camera transforms.
