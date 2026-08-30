# PostProcess、FXAA、ColorGrading 与 Present 边界

> 状态：2026-08-30 已按当前代码更新。PostProcess 不再直接写默认 backbuffer；FXAA 与 LUT ColorGrading 是可切换的 LDR 节点，Present 是唯一的最终呈现边界。
> 关联：[`frame-graph-mvp.md`](frame-graph-mvp.md)、[`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md)。

## 1. 管线位置

Forward：

```text
Shadow → ForwardOpaque → Forward2DOpaque → DepthHaze(no-op)
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → FXAA(view 17, FxaaColor)
       → ColorGrading(view 4, ColorGradedColor)
       → Present(view 16, backbuffer) → UI(view 255)
```

Deferred：

```text
Shadow → Skybox → GBuffer → SSAO → Lighting → DepthHaze
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → FXAA(view 17, FxaaColor)
       → ColorGrading(view 4, ColorGradedColor)
       → Present(view 16, backbuffer) → UI(view 255)
```

Editor 选中轮廓在 Transparent 后以 view 253 写双通道遮罩：Alpha 是所选网格的完整投影，RGB 是通过场景深度测试的可见覆盖；view 254 仅从 Alpha 计算固定两像素外边界，再用 RGB 抑制被遮挡段。这样前景物体既不会在大型地面上打洞并被误描边，外框也不会穿透前景；之后才进入 Bloom/PostProcess。Present 后、UI 前的 EditorOverlay 仅绘制 view 251 的方向轴；view 252 保留兼容用途。

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
                                           ▼
                                  FXAA, view 17
                                           │
                                FrameGraph FxaaColor
                                           │
                              PresentSource semantic
                                           │
                                           ▼
                           LUT ColorGrading, view 4
                                           │
                           FrameGraph ColorGradedColor
                                           │
                              PresentSource semantic
                                           │
                                           ▼
                                  Present, view 16
                                           │
                                           ▼
                              default backbuffer viewport rect
```

- FrameGraph 拥有 `FinalLdrColor` 以及启用时的 `FxaaColor`、`ColorGradedColor` 物理 RenderTarget lease；四个 Pass 都不拥有私有 FBO。
- PostProcess 只使用 viewport-local 的 `(0,0,width,height)` 写 FinalLdrColor。
- Present 才应用 Editor Game View 的 `(viewportX, viewportY)` 偏移并写默认 backbuffer。
- `FrameGraph::markProduced(FinalLdrColor)` 只在 PostProcess 成功 submit 后置位；`beginFrame()` 会清零。Present 不会把跨帧复用的有效 handle 误认为本帧产物。
- `FgSemantic::PresentSource` 每帧先指向 `FinalLdrColor`。FXAA 与 ColorGrading 都只在真实 submit 并标记各自输出为本帧产物后提升 semantic；任一节点的 shader、binding、geometry、LUT、FBO 或 attachment 失败时，Present 自动保留最近一次成功的 LDR 输入，不会黑屏。

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

## 4. FXAA 契约

FXAA cache key：`fxaa_311_luma_v1`。

- 输入为已经 tone-map 与 gamma encode 的 display-referred `FinalLdrColor`，输出为同尺寸 RGBA8 `FxaaColor`；
- shader 使用中心与四角 luma 检测、方向搜索和 FXAA 3.11 风格的 A/B 候选滤波，保留中心 alpha；
- 所有 offset UV 都显式 clamp 到 `[0,1]`，不依赖 RenderTarget 的 sampler address mode；
- `inverseViewport.xy=(1/width,1/height)`，resize 后每帧按实际 Game View 尺寸上传；
- `Renderer::setFxaaEnabled(bool)` 默认开启，关闭后不声明/分配 FxaaColor，也不产生 fullscreen draw；
- 程序获取失败按 120 帧有界重试；`PresentSource` 的成功后提升保证 FXAA 故障不会吞掉最终画面。

## 5. LUT ColorGrading 契约

ColorGrading cache key：`color_grading_lut2d_32_v2`。v2 将 LUT 坐标计算拆成标量，规避 Phoskia 当前 HLSL emitter 将 `clamp(vec3, vec3, vec3)` 错误推断为标量、导致 D3D11/D3D12 `s_5_0` 编译失败的问题。

- 输入为当前 `PresentSource`，因此 FXAA 关闭或失败时会直接处理 `FinalLdrColor`；
- 32³ RGB LUT 按蓝色 slice 横向展开为 `1024×32 RGBA8` 2D strip，兼容当前 portable `texture2d` 后端；
- shader 对 R/G 维使用硬件双线性采样，并在相邻 B slice 之间显式插值，得到等价的三线性 LUT 查询；
- 内置 Neutral、Warm、Cool、Cinematic 四种程序化预设，强度在 `[0,1]` 混合原色与 LUT 结果；
- Renderer 默认关闭，预设 Warm、强度 0.75；关闭、Neutral 或零强度时不声明/分配 `ColorGradedColor`，也不产生 fullscreen draw；
- Neutral 是显式 bypass，而不是“很弱的调色”。Editor 将它标为 `Neutral (Bypass)`；用户从 Neutral 状态开启效果时会自动切到 Warm，旧的 `enabled + Neutral` 偏好也在加载时迁移到 Warm，保证开关具有可观察结果；
- LUT 仅在预设变化后重建，程序获取失败按 120 帧有界重试；成功 submit 后才把 `PresentSource` 提升到 `ColorGradedColor`。
- Pass 在首次成功 submit 时记录 view、尺寸、预设和强度；FrameGraph source/target 无效分别诊断。共享 RenderTargetPool 若底层 framebuffer 创建失败，会在两帧 quarantine 之外淘汰一个最旧空闲目标并重试一次，避免 UI/viewport 多尺寸历史目标占满 bgfx handle 后使 SSAO 与 ColorGrading 同时失效。

当前没有外部 `.cube`/图片 LUT 资产导入接口；内置预设先用于验证 Pass、色彩链路和编辑器交互，后续资源接口可复用同一 2D-strip GPU 契约。

## 6. Present 契约

Present cache key：`present_final_ldr_blit_v1`。

Present 是最小的一纹理 fullscreen copy：

- 输入只来自 `FgSemantic::PresentSource`；
- 要求 `PresentSource` 当前指向的 logical resource 本帧 production latch 为真；
- 输出固定为默认 backbuffer；
- 不做曝光、tone-map、gamma、调色或 UI 合成；
- adapter 未初始化、Noop backend、零 viewport、semantic/attachment 无效时返回 0；
- program、binding 或 fullscreen geometry 不可用时 fail-close 并保留诊断日志。

PostProcess、FXAA、ColorGrading 与 Present 共用 `FullscreenPassGeometry` 的超大三角形创建、绑定、销毁及 view 配置逻辑，但各 Pass 仍独立拥有自己的 GPU buffer handle，生命周期不会隐式耦合。

## 7. View ID 保留表

| View | Owner |
|---:|---|
| 4 | ColorGrading → ColorGradedColor（显式排在 FXAA 与 Present 之间） |
| 15 | PostProcess → FinalLdrColor |
| 16 | Present → backbuffer |
| 17 | FXAA → FxaaColor（显式排在 Present 之前） |
| 18–25 | Shadow atlas slots |
| 26–249 | UI offscreen layer/RenderTarget |
| 250 | GBufferDebug |
| 251 | Editor orientation axis |
| 252 | EditorOverlay 兼容保留 |
| 253 | Selection visibility mask |
| 254 | Selection screen-space composite |
| 255 | UI main composite |

`RenderViewOrder` 显式保证生产依赖顺序；新增 Pass 不得仅依据数字大小猜测执行次序，也不得复用上述区间。

## 8. 后续 Pass 的接入方式

后 tone-map、低动态范围效果应插在 PostProcess 与 Present 之间：

```text
FinalLdrColor → FXAA → ColorGrading → OtherLdrEffect → NewLdrColor
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

## 9. 验收

- `Test_PresentPass`：ABI、管线顺序、view 区间、current-frame latch、Noop、运行时 shader 编译。
- `Test_FXAA`：append-only ABI、默认/Deferred/Editor 顺序、运行时开关、失败回退、边缘 clamp、生产 shader 编译与 binding 反射。
- `Test_ColorGrading`：append-only ABI、管线/view 顺序、参数持久化、LUT 布局、semantic 成功后提升、Noop/双重销毁、生产 shader 编译与 binding 反射。
- `Test_FinalPP_S1c`、`Test_PostProcess_F5`、Bloom/DepthHaze/SSAO 套件：上游合成与来源选择回归。
- D3D11/D3D12 真机：确认 FXAA on/off 的轮廓差异，ColorGrading 四预设/强度、画面方向、Editor viewport rect、resize、Bloom 开关、Debug/EditorOverlay/UI 遮挡顺序。

## 10. 当前未实现

- 精确 sRGB OETF、dithering 与 HDR swapchain 输出；
- 外部 `.cube`/图片 LUT 导入、SMAA；
- DOF、MotionBlur、TAA 等历史/深度类效果；
- 基于 GPU capture 的带宽与 RenderTarget alias 优化。
