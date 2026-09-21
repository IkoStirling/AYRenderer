# TAA history rejection investigation — 2026-09-21

Status: **cause reproduced; production fix not applied in this round**.

## Question and scope

The user accepts small temporal ripple, but not abrupt dots/protrusions at a
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
Remove-Item Env:AY_TAA_PROBE_REJECT_TINY_TAPS -ErrorAction SilentlyContinue
& .\out\build\windows-debug-vs2026-insider\AYRuntime\AYRenderer\unittest\AYRenderer_Test.exe AYRenderer_TAARejectionGpu
$env:AY_TAA_PROBE_REJECT_TINY_TAPS = '1'
& .\out\build\windows-debug-vs2026-insider\AYRuntime\AYRenderer\unittest\AYRenderer_Test.exe AYRenderer_TAARejectionGpu
```

Output `reasons` bins: accepted, no history, invalid Motion, extreme footprint,
previous depth outside clip range, no compatible history support, other gate
(including the test-only support guard). Watched-pixel detail mode 7 reports
surface UV / reconstructed history red / current red; mode 9 reports history
fraction / depth tolerance / surviving support. Detail output is RGBA16F.

The tests intentionally characterize the known baseline failure at 127x127.
When production is fixed, replace those baseline-failure assertions with the
new invariant; do not preserve the bug to satisfy the characterization.

## Next fix boundaries

Preserve surviving tap support as history confidence instead of converting any
nonzero support into full confidence. Verify tiny-support handling and gradual
feedback separately from depth rejection; do not globally relax depth checks.
Check true subpixel motion, disocclusion and thin geometry before adopting a
hard support threshold. Complete user-camera verification after implementation.

The probe reads back between stages and uploads recursive history. It does not
replace the separate GPU-resident ping-pong test or Editor end-to-end capture.
It also does not test transparent layering, complete lighting, cutout/skinned
depth replay, or all camera transforms.
