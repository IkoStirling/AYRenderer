# PostProcess 与 Present 边界

> 状态：2026-08-30 已按当前代码更新。PostProcess 不再直接写默认 backbuffer；Present 是唯一的最终呈现边界。
> 关联：[`frame-graph-mvp.md`](frame-graph-mvp.md)、[`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md)。

## 1. 管线位置

Forward：

```text
Shadow → ForwardOpaque → Forward2DOpaque → DepthHaze(no-op)
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → Present(view 16, backbuffer) → UI(view 255)
```

Deferred：

```text
Shadow → Skybox → GBuffer → SSAO → Lighting → DepthHaze
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → Present(view 16, backbuffer) → UI(view 255)
```

Editor 管线在 Present 后、UI 前插入 EditorOverlay。GBufferDebug 使用 view 250，EditorOverlay 使用 view 251/252，因此最终 GPU 顺序为 Present → GBufferDebug（若启用）→ EditorOverlay → UI。

## 2. 数据流与所有权

```text
Forward sceneFbo / Deferred LightingOutput / current-frame HazeColor
                              │
                              ▼
                    SceneColorPipeline
                              │ HDR RGBA16F
                 ┌────────────┴────────────┐
                 ▼                         ▼
          BloomExtract/Blur          PostProcess
                 │                         │
                 └──── bloom HDR ──────────┤
                                           ▼
                              FrameGraph FinalLdrColor
                                      RGBA8, view 15
                                           │
                               PresentSource semantic
                                           │
                                           ▼
                                  Present, view 16
                                           │
                                           ▼
                              default backbuffer viewport rect
```

- FrameGraph 拥有 `FinalLdrColor` 的物理 RenderTarget lease；PostProcess 和 Present 都不拥有私有 FBO。
- PostProcess 只使用 viewport-local 的 `(0,0,width,height)` 写 FinalLdrColor。
- Present 才应用 Editor Game View 的 `(viewportX, viewportY)` 偏移并写默认 backbuffer。
- `FrameGraph::markProduced(FinalLdrColor)` 只在 PostProcess 成功 submit 后置位；`beginFrame()` 会清零。Present 不会把跨帧复用的有效 handle 误认为本帧产物。
- `FgSemantic::PresentSource` 当前指向 `FinalLdrColor`。后续 FXAA、ColorGrading 等节点可产出新的 LDR 资源并提升该 semantic，而无需改 PostProcess 或 Present 的端点职责。

## 3. PostProcess 合成契约

主程序 cache key：`postprocess_tonemap_aces_v12_sanitized_params_fs`。

| 输入 | 作用 |
|---|---|
| `sceneColor` | SceneColorPipeline 选出的 HDR 主场景色 |
| `bloomTexture` | 仅在 BloomBlur 本帧真实产出且 attachment 有效时使用 |
| `bloomStrength` | 清洗后的 bloom 强度；输入无效时强制为 0 |
| `exposure` | `[0,64]` 内的曝光倍数，非有限值回退 1 |
| `tonemapMode` | None / Reinhard / ACES |
| `gammaParams` | `[0.1,8]` 内的显示 gamma，非有限值回退 2.2 |

核心顺序：HDR 场景与 HDR bloom 按同一曝光域合成，再 tone-map，最后 gamma encode，输出 display-referred RGBA8。独立 fallback 程序只声明 `sceneColor`，主程序恢复采用有界重试，避免 shader 获取失败时逐帧编译。

## 4. Present 契约

Present cache key：`present_final_ldr_blit_v1`。

Present 是最小的一纹理 fullscreen copy：

- 输入只来自 `FgSemantic::PresentSource`；
- 要求 `PresentSource` 当前指向的 logical resource 本帧 production latch 为真；
- 输出固定为默认 backbuffer；
- 不做曝光、tone-map、gamma、调色或 UI 合成；
- adapter 未初始化、Noop backend、零 viewport、semantic/attachment 无效时返回 0；
- program、binding 或 fullscreen geometry 不可用时 fail-close 并保留诊断日志。

PostProcess 与 Present 共用 `FullscreenPassGeometry` 的超大三角形创建、绑定、销毁及 view 配置逻辑，但各 Pass 仍独立拥有自己的 GPU buffer handle，生命周期不会隐式耦合。

## 5. View ID 保留表

| View | Owner |
|---:|---|
| 15 | PostProcess → FinalLdrColor |
| 16 | Present → backbuffer |
| 17 | 当前保留 |
| 18–25 | Shadow atlas slots |
| 26–249 | UI offscreen layer/RenderTarget |
| 250 | GBufferDebug |
| 251 | Editor orientation axis |
| 252 | Editor selection outline |
| 255 | UI main composite |

`RenderViewOrder` 显式保证生产依赖顺序；新增 Pass 不得仅依据数字大小猜测执行次序，也不得复用上述区间。

## 6. 后续 Pass 的接入方式

后 tone-map、低动态范围效果应插在 PostProcess 与 Present 之间：

```text
FinalLdrColor → FXAA/ColorGrading/... → NewLdrColor
                                      └→ PresentSource
```

每个节点需要：

1. append-only 新增 `FgResourceId` 与需要的 semantic；
2. 在 FrameGraph 声明 read/write 和目标格式；
3. 成功 submit 后发布 current-frame production latch；
4. 仅在完整生产链有效时提升 `PresentSource`；
5. 把 `RenderPassSlot` append 到 ABI 尾部，并在默认/Deferred 管线中明确位置；
6. 增加生产 shader 编译、view 唯一性、Noop、resize、teardown 与 stale-handle 测试。

TAA、运动模糊等历史类效果还需要稳定 motion vector、jitter、history ping-pong 和相机切换失效规则，不能按普通单帧 fullscreen Pass 直接接入。

## 7. 验收

- `Test_PresentPass`：ABI、管线顺序、view 区间、current-frame latch、Noop、运行时 shader 编译。
- `Test_FinalPP_S1c`、`Test_PostProcess_F5`、Bloom/DepthHaze/SSAO 套件：上游合成与来源选择回归。
- D3D11/D3D12 真机：确认画面方向、Editor viewport rect、resize、Bloom 开关、Debug/EditorOverlay/UI 遮挡顺序。

## 8. 当前未实现

- 精确 sRGB OETF、dithering 与 HDR swapchain 输出；
- LUT ColorGrading、FXAA/SMAA；
- DOF、MotionBlur、TAA 等历史/深度类效果；
- 基于 GPU capture 的带宽与 RenderTarget alias 优化。
