# AYRenderer R6-6 D3D11 final capture report — 2026-09-23

## Scope and setup

- Tool: RenderDoc 1.46, D3D11, API validation enabled.
- Host: `AYEditorShell_Demo --renderer d3d11`, editor deferred pipeline, 4x MSAA.
- Scene: opaque reference cube and ground, transparent cube, skybox, SSAO,
  Depth Haze, Bloom, TAA and editor selection outline. The shadow-atlas case
  temporarily replaces the normal lights with one directional and one spot
  shadow caster.
- Capture prefix: `out/captures/renderdoc/r6-6-final-2026-09-23_*`. The binary
  `.rdc`, exported XML and thumbnails are local validation artifacts and are
  intentionally not tracked in Git.
- The capture requests are spaced by five application frames. This matters
  because RenderDoc services `TriggerCapture` asynchronously; the wider spacing
  prevents one request from inheriting the next diagnostic channel or filename.

## Results

### Selection outline

The selected silhouette now has two raster grids in one RGBA8 mask:

- view 244 writes only alpha using the unjittered camera and no depth target;
- view 253 writes only RGB visibility using the jittered scene projection and
  the current GBuffer depth;
- view 254 samples alpha at stable output UV and RGB at jitter-aligned UV, then
  composites after Present and before the orientation widget/UI.

The captured order is
`... Transparent(9) -> 244 -> 253 -> Bloom/Post/TAA/Present -> 254 -> 251 -> 255`.
Views 244, 253 and 254 each contain one draw in the reference scene. This
removes the previous reverse-offset-only design: TAA jitter no longer changes
the raster coverage used to construct the orange rim, while visibility still
matches the jittered depth buffer.

### Resize, camera cut and effect lifecycle

- `resize_first_frame_frame42` and `resize_settled_frame52` use the resized
  viewport resource set consistently. The first captured post-resize frame
  does not mix old and new attachment generations; the settled frame reuses
  the new generation.
- `camera_cut_frame62` contains the complete TAA chain and a clean final image.
  The cut is issued as a real host-camera discontinuity; renderer unit/GPU
  tests cover the associated persistent-history invalidation contract.
- `effects_off_frame72` has view order
  `1,18,0,6,7,8,9,244,253,15,16,254,251,255`. Motion(3), SSAO(14),
  Haze(13), Bloom(10-12) and TAA(5) are absent.
- `effects_reopen_first_frame_frame82` and
  `effects_reopen_settled_frame92` restore
  `1,18,0,6,7,3,14,8,13,9,244,253,10,11,12,15,5,16,254,251,255`.
  The first reopened frame has valid targets and no stale disabled output.

### Raw resources and TAA diagnostics

Distinct captures were produced for Albedo, Normal, World Position, Material,
Depth, Material Model, MotionVectors, SSAO Occlusion, TAA History and Shadow
Atlas. Motion and SSAO were re-captured with the wider request interval and are
visibly distinct, closing the earlier asynchronous filename/channel ambiguity.

The five non-final TAA diagnostic modes were also captured independently:
History rejection, History weight, Clipping difference, Motion vectors and
Reprojected history. Their displayed ranges match the documented meanings;
they are diagnostic values, not error masks.

The multi-light atlas capture has both shadow views 18 and 19 before the main
chain, and channel 9 displays two populated atlas regions. Missing/disabled
resource behavior remains fail-closed through the resource blackboard tests.

### Transparent boundary

`transparent_boundary_static_frame106` and
`transparent_boundary_motion_frame111` are separate captures with a stable
camera followed by a small lateral move across the transparent/opaque boundary.
The final images remain complete. This capture records the current behavior but
does not close `TAA-TRANSPARENCY-01`: transparent color still has no matching
transparent depth/motion surface, so that limitation remains an explicit future
reactive-mask/transparent-motion item.

## D3D12 minimum gate

The same freshly linked editor was started with `--renderer d3d12` and remained
alive through a 15-second startup smoke. The persistent log records renderer
type 3 and successful first dispatches for Shadow, Skybox, GBuffer,
MotionVector, SSAO, Lighting, Transparent, Bloom, PostProcess and TAA.

If presentation bootstrap returns failure, the editor now closes the splash,
writes the persistent-log location, shows a modal renderer-startup error and
shuts the session/device before returning. The R6-6 D3D12 requirement is only
this startup/failure-reporting gate; full D3D11/D3D12 image equivalence remains
separate work.

## Gate decision

R6-6 is complete. The normal chain, raw diagnostic resources, resize,
camera-cut, disable/reopen, multi-light shadow atlas and transparent-boundary
cases now have real D3D11 capture evidence. `TAA-TRANSPARENCY-01` remains an
open documented capability limit rather than an unrecorded release blocker.

