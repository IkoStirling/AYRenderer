# AYRenderer D3D11 Baseline Capture — 2026-09-21

## Scope and evidence

- Renderer commit: `dd6599d`; root commit: `9ab3998`.
- Tool: RenderDoc 1.46 x64, API validation enabled.
- Host: `AYEditorShell_Demo --renderer d3d11` on NVIDIA GeForce RTX 3060 Laptop GPU.
- Captures: frame 24490, 24663, and 24686. Local `.rdc` files are build artifacts under `out/build/windows-debug-vs2026-insider/AYRuntime/AYEditor/temp/` and are intentionally not versioned.
- Frame 24686: 1536×912 window/backbuffer, 1083×594 editor viewport, RGBA8 4×MSAA swapchain. The capture decoded successfully; its final thumbnail showed the complete editor scene rather than a loading or empty frame.
- bgfx overrides RenderDoc's capture key to F11, disables the in-application overlay, and replaces the requested output template with `temp/bgfx`. F12 therefore does nothing in this build.

This report records resource creation and event/binding evidence exported from the captures. It does not claim that every intermediate texture's pixels were visually inspected.

## Normal-frame pass chain

| View | Stage | Viewport | Draws | Observed output |
|---:|---|---:|---:|---|
| 1 / 18 | Shadow clear / first caster slot | 4096² / 1024×2048 | 0 / 1 | RGBA8 encoded atlas plus depth |
| 6 | Skybox | 1083×594 | 1 | RGBA8 backdrop |
| 7 | GBuffer | 1083×594 | 2 | Four MRTs plus D24S8 depth |
| 3 | MotionVector | 1083×594 | 2 | RGBA16F motion/history-support data |
| 14 | SSAO | 1083×594 | 1 | RGBA8 occlusion |
| 8 | Lighting | 1083×594 | 1 | RGBA16F scene color |
| 13 | DepthHaze | 1083×594 | 1 | RGBA16F scene color |
| 9 | Transparent | 1083×594 | 1 | Composites into the Haze scene target with GBuffer depth |
| 253 | Selection mask | 1083×594 | 2 | RGBA8 mask |
| 10 / 11 / 12 | Bloom extract / H / V | 542×297 | 1 each | Three RGBA16F half-resolution targets |
| 15 | PostProcess | 1083×594 | 1 | RGBA8 FinalLdr |
| 5 | TAA resolve | 1083×594 | 1 | RGBA16F history-write / present source |
| 16 | Present | 1083×594 | 1 | RGBA8 4×MSAA swapchain |
| 254 / 251 / 252 / 255 | Selection composite / axis / gizmo-grid / UI | viewport-local / full window | 1 / 3 / 2 / 155 | Backbuffer overlays after Present |

The ordering matches `RenderViewOrder` and no producer/consumer inversion was observed. FXAA, SMAA, ColorGrading, TAA diagnostic view 245, and GBufferDebug view 250 were absent because the captured settings selected TAA Final image and left those optional stages disabled.

## Resource contract evidence

| Resource | Captured size and format | Consumer evidence |
|---|---|---|
| Shadow atlas | 4096×4096 RGBA8 plus depth | Lighting view 8 sampled the atlas |
| GBuffer albedo | 1083×594 RGBA8 | Lighting; normal post chain as declared |
| GBuffer normal | 1083×594 RGBA8 | SSAO and Lighting sampled it |
| GBuffer world position | 1083×594 RGBA16F | SSAO, Lighting, Haze, and TAA sampled it |
| GBuffer material/coverage | 1083×594 RGBA8 | SSAO, Lighting, Haze, and TAA sampled it |
| GBuffer depth | 1083×594 R24G8 typeless, D24S8 DSV | Motion/Transparent/selection reused it; TAA sampled its SRV |
| MotionVectors | 1083×594 RGBA16F | Produced at view 3 and sampled by TAA view 5 |
| SSAO | 1083×594 RGBA8 | Produced at view 14 and sampled by Lighting view 8 |
| LightingColor | 1083×594 RGBA16F | Produced at view 8 and sampled by Haze view 13 |
| Haze/Transparent scene | 1083×594 RGBA16F | Bloom view 10 and PostProcess view 15 sampled it |
| Bloom A/B/final | 542×297 RGBA16F each | H and V consume the previous stage; PostProcess consumes final blur |
| FinalLdr | 1083×594 RGBA8 | TAA view 5 sampled it as current color |
| TAA history A/B | 1083×594 RGBA16F each | Resolve reads one and writes the other; Present reads the write target |

Transparent view 9 uses the same physical RGBA16F scene texture produced by Haze through a distinct framebuffer view and reuses the GBuffer depth texture through its transparent DSV. It does not sample and write the same scene color texture in one draw.

## TAA history ping-pong

| Captured frame | History write texture | History read texture | Result |
|---:|---:|---:|---|
| 24490 | 3632 | 3636 | A ← B |
| 24663 | 3636 | 3632 | B ← A |
| 24686 | 3632 | 3636 | A ← B |

Every resolve also reads FinalLdr, MotionVectors, GBuffer world/material/depth, and Present samples the history texture written by that frame. No capture shows TAA reading and writing the same history texture.

## Diagnostics and open gates

- The capture contained one `Present`, 177 indexed draws and two non-indexed draws; 155 indexed draws belonged to the editor UI. Scene-pass draw counts matched the visible cube/ground/glass and overlay content.
- API validation produced no D3D11 validation failure in the exported capture. The injection log contained expected RenderDoc/bgfx integration notices, including NvAPI interception and the dummy info queue, but no captured device-removal or invalid-resource error.
- Raw intermediate pixel values were not inspected in the RenderDoc texture viewer because automated UI control was unavailable. Binding, dimensions, formats, draw order, final thumbnail, and cross-frame history identities were verified from the replayable capture/XML.
- Still required: raw MRT/Motion/SSAO/History/Shadow pixel inspection; SSAO/Bloom/Haze/TAA disable and re-enable; camera cut; viewport resize and generation replacement; TAA diagnostic modes; multi-light atlas regions; and the known `TAA-TRANSPARENCY-01` movement cases.

Status: **normal-frame D3D11 baseline passed; R6-6 D3D11 gate remains partially open**.
