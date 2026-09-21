# AYRenderer D3D11 Capture Checklist

> R6-6 gate. A unit test, shader compile, ordinary screenshot, or a clean final image is useful evidence but does not replace a frame capture that verifies the actual resources and submissions below.

## 1. Fixed capture setup

- Run `AYEditorShell_Demo` with the D3D11 backend and record the executable revision, viewport size, MSAA state, scene, camera transform, and RenderSettings values.
- Use one stable scene containing opaque geometry, a cutout or skinned object when available, one transparent object, skybox, at least one shadow-casting light, visible SSAO contact, Bloom highlights, and non-zero Depth Haze.
- Enable the renderer statistics overlay and reset its peak counters before the measured sequence. Record FrameGraph compile status/live pass count, blackboard produced/history counts, transient RT current/peak, and per-Pass CPU/GPU timings.
- Capture a baseline frame with all optional diagnostics disabled before enabling an overlay channel.

## 2. MRT and raw resource views

- Channels 0–5: verify Albedo, Normal, World Position, Material, Depth, and Material Model against the selected geometry and viewport bounds.
- Channel 6 MotionVectors: verify a stationary scene, camera movement, rigid-object movement, and skinned movement. Stationary valid geometry should be centered in RG; invalid/background pixels must be distinguishable through B validity.
- Channel 7 SSAO Occlusion: compare disabled, default, and deliberately strong settings. The selected resource must disappear/fail-close when SSAO has no current-frame output rather than showing the prior frame.
- Channel 8 TAA History: verify initial invalid history, settled static history, camera/object movement, camera cut, TAA disable/re-enable, and resize. Pair it with the existing TAA diagnostics modes rather than treating raw history color as resolve correctness.
- Channel 9 Shadow Atlas: verify atlas regions for one and multiple lights, shadow disable/re-enable, and bias changes. Missing sampleable shadow content must fail-close.

## 3. Pass-chain capture

- MRT: inspect formats, dimensions, clear values, writes, and Lighting reads for all five GBuffer attachments.
- Shadow: inspect atlas creation, caster submissions, light-region view/projection data, and Lighting sampling.
- Bloom: inspect bright extraction, horizontal/vertical blur targets, PostProcess input, and the zero-resource path when disabled.
- Depth Haze and SSAO: inspect current-frame inputs/outputs, production latches, and disabled paths without stale reads or invalid-target warnings.
- TAA: inspect current color, motion, history-read/history-write, resolve output, ping-pong transition, camera-cut invalidation, and resize recreation. Include the six existing TAA diagnostic modes.
- Transparent: verify it reads the intended scene/depth resources and remains ordered before temporal/final presentation as designed. Record the known transparent-history limitation tracked as `TAA-TRANSPARENCY-01` rather than silently accepting or closing it.
- Resize: capture at least one viewport size change and verify old transient/history targets are not consumed after generation changes.

## 4. Acceptance record

For each item, save the capture filename, commit, backend, dimensions, relevant settings, observed resource names/formats, and pass/fail result. R6-6 D3D11 capture is closed only when:

- the expected producer and consumer submissions are present in order;
- disabled effects add no unexpected draw or retained transient target after the retention window;
- every consumer reads the intended current generation/current frame or valid persistent history;
- resize, camera cut, toggles, and missing resources fail safely without stale output;
- the final image and diagnostic views agree with the documented resource semantics.

RenderDoc was not installed on the development machine when this checklist was added. The code and automated checks prepare the capture points, but the gate remains open until a real capture is attached to this record.
