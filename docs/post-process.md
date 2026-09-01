# PostProcess、FXAA/SMAA、ColorGrading 与 Present 边界

> 状态：2026-09-01 已按当前代码更新。PostProcess 不再直接写默认 backbuffer；FXAA、SMAA 1x 与 LUT ColorGrading 是可切换的 LDR 节点，Present 是唯一的最终呈现边界。
> 关联：[`frame-graph-mvp.md`](frame-graph-mvp.md)、[`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md)。

## 1. 管线位置

Forward：

```text
Shadow → ForwardOpaque → Forward2DOpaque → DepthHaze(no-op)
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → FXAA(view 17, FxaaColor)
       → SMAA Edge/Weight/Neighborhood(views 247–249, SmaaColor)
       → ColorGrading(view 4, ColorGradedColor)
       → Present(view 16, backbuffer) → UI(view 255)
```

Deferred：

```text
Shadow → Skybox → GBuffer → SSAO → Lighting → DepthHaze
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → FXAA(view 17, FxaaColor)
       → SMAA Edge/Weight/Neighborhood(views 247–249, SmaaColor)
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
                          SMAA edge / weight / blend
                                           │
                   SmaaEdges → SmaaBlendWeights → SmaaColor
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

- FrameGraph 拥有 `FinalLdrColor` 以及启用时的 `FxaaColor`、`SmaaEdges`、`SmaaBlendWeights`、`SmaaColor`、`ColorGradedColor` 物理 RenderTarget lease；各 Pass 都不拥有私有 FBO。
- PostProcess 只使用 viewport-local 的 `(0,0,width,height)` 写 FinalLdrColor。
- Present 才应用 Editor Game View 的 `(viewportX, viewportY)` 偏移并写默认 backbuffer。
- `FrameGraph::markProduced(FinalLdrColor)` 只在 PostProcess 成功 submit 后置位；`beginFrame()` 会清零。Present 不会把跨帧复用的有效 handle 误认为本帧产物。
- `FgSemantic::PresentSource` 每帧先指向 `FinalLdrColor`。FXAA、SMAA 与 ColorGrading 都只在真实 submit 并标记各自输出为本帧产物后提升 semantic；SMAA 只在三个阶段全部提交后发布 `SmaaColor`。任一节点的 shader、binding、geometry、lookup texture、LUT、FBO 或 attachment 失败时，Present 自动保留最近一次成功的 LDR 输入，不会黑屏。

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

FXAA cache key：`fxaa_quality_edge_search_v2`。

- 输入为已经 tone-map 与 gamma encode 的 display-referred `FinalLdrColor`，输出为同尺寸 RGBA8 `FxaaColor`；
- shader 先采样中心与 N/E/S/W，以 `max(0.0312, lumaMax*0.125)` 做低对比度门控；非边缘像素原样返回，不再经过方向模糊；
- 确认边缘后才读取四角，区分水平/垂直边缘，并沿切线以 1/2/4/8 像素有界搜索端点；最终只沿边缘法线移动一次双线性采样，而不是对宽方向核求平均；
- sub-pixel correction 默认 0.50，低于常见的 0.75，以减少纹理和文字变软；中心 alpha 始终保留；
- 所有 offset UV 都显式 clamp 到 `[0,1]`，不依赖 RenderTarget 的 sampler address mode；
- `inverseViewport.xy=(1/width,1/height)`，resize 后每帧按实际 Game View 尺寸上传；`fxaaQuality=(0.125,0.0312,0.50,0.25)` 分别表示相对阈值、最小阈值、sub-pixel 强度与端点判定阈值；
- 质量路径使用 raw bgfx `.sc`，因为当前 Phoskia BGFX emitter 尚不输出 `IfStmt`，无法可靠实现非边缘 early-out 和有界搜索控制流；D3D11/D3D12 共用 `s_5_0` 编译结果；
- 非边缘像素为 5 次纹理读取；边缘最坏约 18 次，仍保持单个 full-resolution draw；
- `Renderer::setFxaaEnabled(bool)` 默认开启，关闭后不声明/分配 FxaaColor，也不产生 fullscreen draw；
- 程序获取失败按 120 帧有界重试；`PresentSource` 的成功后提升保证 FXAA 故障不会吞掉最终画面。

## 5. SMAA 1x 契约

SMAA cache keys：`smaa1x_luma_edge_v1`、`smaa1x_ortho_weights_v2`、`smaa1x_neighborhood_v1`。

- 输入为当前 `PresentSource`，输出为同尺寸 RGBA8 `SmaaColor`；FXAA 与 SMAA 运行时互斥，正常产品路径不会连续执行两种 AA；
- Edge 阶段使用亮度阈值 0.10 与 2.0 倍局部对比度适配，写入 linear/clamp `SmaaEdges`；低对比度与非边缘像素不会进入邻域滤波；
- Blend Weight 阶段沿水平/垂直方向逐像素搜索，单向上限 16 像素，写入 linear/clamp `SmaaBlendWeights`；它执行精确的一像素步进，因此无需参考实现为双像素优化搜索准备的 SearchTex；交叉边在法向偏移 0.25 像素处读取，以通过双线性采样保留 0.25/0.75 端点图案，避免斜向阶梯被错误量化到 AreaTex 零权重块；
- 正交 Area lookup 是启动时按参考算法生成的 `80×80 RG8` 纹理，不依赖 loose asset；来源与 MIT 许可记录在 `THIRD_PARTY_NOTICES.md`；
- Neighborhood 阶段只沿检测到的边缘法线混合两个候选样本，非边缘像素精确返回中心颜色，不做整屏方向模糊；
- 当前是 SMAA 1x 正交路径，包含 crossing-edge 权重与 corner attenuation，但尚未加入高质量预设的 diagonal pattern detection；
- 三个 full-resolution draw 固定使用 view 247、248、249。Renderer 默认关闭、Editor 默认开启；关闭后不声明三个 FrameGraph 目标，也不产生 draw；
- 三个 raw bgfx `.sc` 像素阶段经 Windows `s_5_0` 编译测试，D3D11 与 D3D12 共用同一产物；shader/binding/AreaTex 任一失败时不提升 `PresentSource`。

## 6. LUT ColorGrading 契约

ColorGrading cache key：`color_grading_lut2d_32_v2`。v2 将 LUT 坐标计算拆成标量，规避 Phoskia 当前 HLSL emitter 将 `clamp(vec3, vec3, vec3)` 错误推断为标量、导致 D3D11/D3D12 `s_5_0` 编译失败的问题。

- 输入为当前 `PresentSource`，因此 AA 关闭或失败时会直接处理 `FinalLdrColor`；
- 32³ RGB LUT 按蓝色 slice 横向展开为 `1024×32 RGBA8` 2D strip，兼容当前 portable `texture2d` 后端；
- shader 对 R/G 维使用硬件双线性采样，并在相邻 B slice 之间显式插值，得到等价的三线性 LUT 查询；
- 内置 Neutral、Warm、Cool、Cinematic 四种程序化预设，强度在 `[0,1]` 混合原色与 LUT 结果；
- Renderer 默认关闭，预设 Warm、强度 0.75；关闭、Neutral 或零强度时不声明/分配 `ColorGradedColor`，也不产生 fullscreen draw；
- Neutral 是显式 bypass，而不是“很弱的调色”。Editor 将它标为 `Neutral (Bypass)`；用户从 Neutral 状态开启效果时会自动切到 Warm，旧的 `enabled + Neutral` 偏好也在加载时迁移到 Warm，保证开关具有可观察结果；
- LUT 仅在预设变化后重建，程序获取失败按 120 帧有界重试；成功 submit 后才把 `PresentSource` 提升到 `ColorGradedColor`。
- Pass 在首次成功 submit 时记录 view、尺寸、预设和强度；FrameGraph source/target 无效分别诊断。共享 RenderTargetPool 若底层 framebuffer 创建失败，会在两帧 quarantine 之外淘汰一个最旧空闲目标并重试一次，避免 UI/viewport 多尺寸历史目标占满 bgfx handle 后使 SSAO 与 ColorGrading 同时失效。

当前没有外部 `.cube`/图片 LUT 资产导入接口；内置预设先用于验证 Pass、色彩链路和编辑器交互，后续资源接口可复用同一 2D-strip GPU 契约。

## 7. Present 契约

Present cache key：`present_final_ldr_blit_v1`。

Present 是最小的一纹理 fullscreen copy：

- 输入只来自 `FgSemantic::PresentSource`；
- 要求 `PresentSource` 当前指向的 logical resource 本帧 production latch 为真；
- 输出固定为默认 backbuffer；
- 不做曝光、tone-map、gamma、调色或 UI 合成；
- adapter 未初始化、Noop backend、零 viewport、semantic/attachment 无效时返回 0；
- program、binding 或 fullscreen geometry 不可用时 fail-close 并保留诊断日志。

PostProcess、FXAA、SMAA、ColorGrading 与 Present 共用 `FullscreenPassGeometry` 的超大三角形创建、绑定、销毁及 view 配置逻辑，但各 Pass 仍独立拥有自己的 GPU buffer handle，生命周期不会隐式耦合。

## 8. View ID 保留表

| View | Owner |
|---:|---|
| 4 | ColorGrading → ColorGradedColor（显式排在 FXAA 与 Present 之间） |
| 15 | PostProcess → FinalLdrColor |
| 16 | Present → backbuffer |
| 17 | FXAA → FxaaColor（显式排在 Present 之前） |
| 18–25 | Shadow atlas slots |
| 26–246 | UI offscreen layer/RenderTarget（每帧最多 221 次 retained repaint） |
| 247 | SMAA edge detection → SmaaEdges |
| 248 | SMAA blend weights → SmaaBlendWeights |
| 249 | SMAA neighborhood blending → SmaaColor |
| 250 | GBufferDebug |
| 251 | Editor orientation axis |
| 252 | EditorOverlay 兼容保留 |
| 253 | Selection visibility mask |
| 254 | Selection screen-space composite |
| 255 | UI main composite |

`RenderViewOrder` 显式保证生产依赖顺序；新增 Pass 不得仅依据数字大小猜测执行次序，也不得复用上述区间。

## 9. 后续 Pass 的接入方式

后 tone-map、低动态范围效果应插在 PostProcess 与 Present 之间：

```text
FinalLdrColor → FXAA/SMAA → ColorGrading → OtherLdrEffect → NewLdrColor
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

## 10. 验收

- `Test_PresentPass`：ABI、管线顺序、view 区间、current-frame latch、Noop、运行时 shader 编译。
- `Test_FXAA`：append-only ABI、默认/Deferred/Editor 顺序、运行时开关、失败回退、对比度门控、水平/垂直边缘分类、1/2/4/8 搜索上界、边缘 clamp、质量参数及 D3D11/D3D12 `s_5_0` 生产 shader 编译。
- `Test_SMAA`：append-only ABI、三阶段管线/view 顺序、FXAA 互斥、程序化 Area lookup、45° 阶梯交叉边非零权重回归、semantic 事务提升、Noop/双重销毁、shader 契约及三个像素阶段的 D3D11/D3D12 `s_5_0` 编译。
- `Test_ColorGrading`：append-only ABI、管线/view 顺序、参数持久化、LUT 布局、semantic 成功后提升、Noop/双重销毁、生产 shader 编译与 binding 反射。
- `Test_FinalPP_S1c`、`Test_PostProcess_F5`、Bloom/DepthHaze/SSAO 套件：上游合成与来源选择回归。
- D3D11/D3D12 真机：确认 FXAA/SMAA/off 的轮廓与清晰度差异，ColorGrading 四预设/强度、画面方向、Editor viewport rect、resize、Bloom 开关、Debug/EditorOverlay/UI 遮挡顺序。

## 11. 当前未实现

- 精确 sRGB OETF、dithering 与 HDR swapchain 输出；
- 外部 `.cube`/图片 LUT 导入、SMAA diagonal/high-preset 扩展；
- DOF、MotionBlur、TAA 等历史/深度类效果；
- 基于 GPU capture 的带宽与 RenderTarget alias 优化。
