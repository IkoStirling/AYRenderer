# PostProcess、TAA/FXAA/SMAA、ColorGrading 与 Present 边界

> 状态：2026-09-02 已按当前代码更新。PostProcess 不再直接写默认 backbuffer；Deferred TAA、FXAA、SMAA 1x 与 LUT ColorGrading 是可切换的 LDR 节点，Present 是唯一的最终呈现边界。TAA 与 MotionVector 使用 Phoskia；FXAA/SMAA 是有明确能力缺口与跨后端测试约束的 raw `.sc` 兼容例外。
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
Shadow → Skybox → GBuffer → MotionVector(on demand) → SSAO → Lighting → DepthHaze
       → Transparent → BloomExtract → BloomBlur
       → PostProcess(view 15, FinalLdrColor)
       → TAA(view 5, persistent TaaColor，Deferred-only)
       → FXAA(view 17, FxaaColor)
       → SMAA Edge/Weight/Neighborhood(views 247–249, SmaaColor)
       → ColorGrading(view 4, ColorGradedColor)
       → Present(view 16, backbuffer) → UI(view 255)
```

Editor 选中轮廓在 Transparent 后使用同一 RGBA8 mask 的两套栅格：view 244 以未 jitter 投影且不绑定深度，只写 Alpha 完整 silhouette；view 253 以场景相同的 jitter 投影并借用 GBuffer depth，只写 RGB 可见覆盖。view 254 延后到 Present 之后，Alpha 始终按稳定输出 UV 采样，RGB 则按当前 jitter 对齐 UV 采样，再以八方向环形覆盖率重建柔和的固定两像素外边界并抑制遮挡段。不能再退回单一 jittered silhouette 后反向偏移的方案，因为偏移不能撤销光栅覆盖率随 jitter 相位改变。选中框不进入 Bloom/PostProcess/TAA history；之后 view 251 绘制方向轴、view 252 绘制 Transform Gizmo，二者也使用未 jitter 的相机投影。

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
                         TAA / FXAA / SMAA（三选一）
                          │       │        │
                  persistent  FxaaColor  SmaaColor
                   TaaColor       │        │
                          └───────┬┴────────┘
                                  ▼
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

- FrameGraph 拥有 `FinalLdrColor` 以及启用时的 `FxaaColor`、`SmaaEdges`、`SmaaBlendWeights`、`SmaaColor`、`ColorGradedColor` 物理 RenderTarget lease。TAA 是例外：`TAAPass` 自己持有两块跨帧 `RGBA16F` history FBO，每帧只把当前写目标以 `TaaColor` 外部资源导入 FrameGraph。
- PostProcess 只使用 viewport-local 的 `(0,0,width,height)` 写 FinalLdrColor。
- Present 才应用 Editor Game View 的 `(viewportX, viewportY)` 偏移并写默认 backbuffer。
- `FrameGraph::markProduced(FinalLdrColor)` 只在 PostProcess 成功 submit 后置位；`beginFrame()` 会清零。Present 不会把跨帧复用的有效 handle 误认为本帧产物。
- `FgSemantic::PresentSource` 每帧先指向 `FinalLdrColor`。TAA、FXAA、SMAA 与 ColorGrading 都只在真实 submit 并标记各自输出为本帧产物后提升 semantic；SMAA 只在三个阶段全部提交后发布 `SmaaColor`。任一节点的 shader、binding、geometry、history、lookup texture、LUT、FBO 或 attachment 失败时，Present 自动保留最近一次成功的 LDR 输入，不会黑屏。

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

FXAA cache key：`fxaa_chroma_edge_search_v3`。

- 输入为已经 tone-map 与 gamma encode 的 display-referred `FinalLdrColor`，输出为同尺寸 RGBA8 `FxaaColor`；
- shader 先采样中心与 N/E/S/W，同时计算亮度范围和 RGB 最大通道差，以两者最大值通过 `max(0.0312, lumaMax*0.125)` 低对比度门控；这能保留原亮度检测的稳定性，并覆盖橙/灰绿等颜色差明显但亮度接近的边缘；非边缘像素原样返回，不再经过方向模糊；
- 确认边缘后才读取四角，区分水平/垂直边缘，并沿切线以 1/2/4/8 像素有界搜索端点；最终只沿边缘法线移动一次双线性采样，而不是对宽方向核求平均；
- sub-pixel correction 默认 0.50，低于常见的 0.75，以减少纹理和文字变软；中心 alpha 始终保留；
- 所有 offset UV 都显式 clamp 到 `[0,1]`，不依赖 RenderTarget 的 sampler address mode；
- `inverseViewport.xy=(1/width,1/height)`，resize 后每帧按实际 Game View 尺寸上传；`fxaaQuality=(0.125,0.0312,0.50,0.25)` 分别表示相对阈值、最小阈值、sub-pixel 强度与端点判定阈值；
- 质量路径是受控的 raw bgfx `.sc` 兼容例外：Phoskia 已能输出 `if`，但尚无用户函数，material `return` 也表示写输出槽而非真正终止控制流；直接迁移会改变非边缘 early-out 与有界搜索的语义。D3D11/D3D12 共用经过测试的 `s_5_0` 编译结果；
- 非边缘像素为 5 次纹理读取；边缘最坏约 18 次，仍保持单个 full-resolution draw；
- `Renderer::setFxaaEnabled(bool)` 默认开启，关闭后不声明/分配 FxaaColor，也不产生 fullscreen draw；
- 程序获取失败按 120 帧有界重试；`PresentSource` 的成功后提升保证 FXAA 故障不会吞掉最终画面。

## 5. SMAA 1x High 契约

SMAA cache keys：`smaa1x_color_edge_v2`、`smaa1x_high_weights_v3`、`smaa1x_neighborhood_v2`。

- 输入为当前 `PresentSource`，输出为同尺寸 RGBA8 `SmaaColor`；FXAA 与 SMAA 运行时互斥，正常产品路径不会连续执行两种 AA；
- Edge 阶段采用官方 SMAA color-edge 路径，以 RGB 最大通道差、阈值 0.10 与 2.0 倍局部对比度适配写入 linear/clamp `SmaaEdges`；它不会漏掉等亮异色轮廓，低对比度与非边缘像素仍不会进入邻域滤波；
- Blend Weight 阶段执行 High 预设的双像素优化正交搜索（单向 16 步）、对角搜索（单向 8 步）和 25% corner rounding，写入 linear/clamp `SmaaBlendWeights`；交叉边保留参考实现的四分之一像素 bilinear decode；
- 完整 Area lookup 在启动时生成 `160×560 RG8`：左半包含 7 组正交 subpixel pattern，右半包含 5 组 diagonal pattern；Search lookup 生成 `64×16 R8`，字节哈希与官方 `SearchTex.h` 一致。对角 Area 使用解析式半平面覆盖计算，抽样值与官方 30×30 参考生成器保持量化容差；两者都不依赖 loose asset，来源与 MIT 许可记录在 `THIRD_PARTY_NOTICES.md`；
- Neighborhood 阶段只沿检测到的边缘法线混合两个候选样本，非边缘像素精确返回中心颜色，不做整屏方向模糊；
- 当前是完整的 SMAA 1x High 单帧路径，包含 diagonal pattern detection、SearchTex 长度校正、crossing-edge 权重和官方 corner detection；它仍不包含时间域重投影，因此无法消除运动中的 subpixel/highlight shimmer；
- 三个 full-resolution draw 固定使用 view 247、248、249。Renderer 默认关闭、Editor 默认开启；关闭后不声明三个 FrameGraph 目标，也不产生 draw；
- 三个像素阶段是受控的 raw bgfx `.sc` 兼容例外：High 参考实现依赖多组用户函数、C 风格有界循环和嵌套分支，当前 Phoskia 无法等价表达。它们经 Windows `s_5_0` 编译测试，D3D11 与 D3D12 共用同一产物；shader/binding/AreaTex/SearchTex 任一失败时不提升 `PresentSource`。

## 6. TAA 契约

TAA cache key：`taa_phoskia_motion_vectors_ycocg_v5`。

- 仅挂载在 Deferred / Editor Deferred 管线，顺序为 `PostProcess → TAA → FXAA → SMAA → ColorGrading → Present`；运行时 TAA、FXAA、SMAA 三选一。Renderer 默认仍使用 FXAA，Editor 可在 Render Settings 选择 `TAA (Deferred)`；
- 场景投影使用标准顺序的 8 样本 Halton(2,3) 亚像素 jitter，分布缩放为 0.75，使小尺寸 Editor viewport 的最大位移约束在三分之一像素附近。保留标准顺序可维持任意前缀的低差异分布，避免人为按空间路径重排后产生前半周期/后半周期的位置偏置。静态/运动 history feedback 分别为 0.92/0.65；只有 fullscreen geometry、生产 shader 与两块 history FBO 全部准备成功时才应用 jitter，避免“抖动存在但 resolve 缺失”；
- 两块全分辨率 `RGBA16F` history FBO 由 Pass 持久拥有并 ping-pong；resize、pipeline rebuild、开关切换和相机 cut 会失效历史并从样本 0 重启；
- 当前帧 `FinalLdrColor` 的 3×3 邻域转换到 YCoCg，历史颜色在邻域 min/max 内限幅，再按屏幕运动量和超出局部亮度跨度的异常差异动态降低反馈。正常高反差斜边的逐样本覆盖变化不会被误判成 disocclusion；首帧、越界、无 geometry coverage 或无有效上一帧时直接写当前颜色；
- opaque 几何优先读取 MotionVector RG16F，以 `previousUV = currentUV - velocity` 重投影刚体、Transform、骨骼与相机运动；Pass 不可用时回退到 GBuffer RT2 world position 与上一帧 jittered view-projection，保证静态几何仍可运行。覆盖位来自 RT3.a；相机完全稳定时，无 coverage 像素按前后两帧 jitter 差值重投影，使轮廓外侧与天空/背景样本也能参与时间积累。相机移动时关闭该背景回退，避免没有 world position 的天空拖影；
- 透明物体仍不写 GBuffer depth/coverage，但 MotionVectorPass 已按可见透明几何重放对象/骨骼 velocity，并在现有 RGBA16F A 通道打包 reactive 标志。TAA 对 reactive 像素使用透明 velocity、跳过 opaque depth 层匹配并将 history feedback 限制为正常值的 15%；这能抑制常规透明拖影，但不是多层透明深度排序或折射专用时域解法；
- shader 使用 Phoskia，经标准 `ShaderResourcePool::acquire` 进入源码缓存与磁盘二进制缓存；分支采用单一最终输出赋值，避免把 material `return` 误当作早退。生产源由 Windows `s_5_0` 编译测试覆盖，D3D11/D3D12 共用该路径。shader/FBO/GBuffer 任一依赖失败时不提升 `PresentSource`，并保持未 jitter 的稳定回退。

### 6.1 Motion Vector 契约

- append-only `RenderPassSlot::MotionVector=20`，Deferred-only，固定顺序 `GBuffer(view 7) → MotionVector(view 3) → SSAO(view 14) → Lighting(view 8)`；view 3 与 Forward transparent 复用但两条管线互斥；
- 仅当 TAA 本帧准备成功时分配/执行。Pass 拥有一张全分辨率 point/clamp `RGBA16F` motion texture 和 non-owning FBO shell，借用 GBuffer D24S8；关闭 TAA、resize、MSAA/reset、pipeline rebuild 与 shutdown 都会先释放 shell，再允许 GBuffer 销毁附件；
- 依次重放 opaque、alpha-cutout 与 transparent 几何，执行 color-only `LEQUAL`，不清深度、不写深度；透明部分沿用 TransparentPass 的稳定远到近顺序。RG 以 UV 单位编码 `currentUV - previousUV`，B 保存 previous depth，A 的 `[0,2)` 保存 validity/footprint，`+2` 表示 reactive transparent；
- `DrawItem::motionObjectId` 为宿主提供稳定身份，零表示兼容静态路径，只生成相机速度。非零对象按 `objectId + meshId` 保存上一帧 world 与完整骨骼姿态；分段 palette 只在上传时 remap，历史不保存借用指针；
- 只有连续上一帧、相同 mesh 和相同完整 skeleton 数量才接受历史。新对象、重现对象、姿态布局变化或切换效果时写 `(2,2)` 越界哨兵，使 TAA 的 UV 门禁拒绝旧 history；120 帧未见的条目被清理；
- AYEntity 刚体、分段蒙皮与 whole-mesh fallback 都提交混入 World 地址、entity id 和 handle generation 的稳定 ID，Edit/Play 场景或回收实体不会串用历史；
- rigid/skinned 与 alpha-cutout 程序均使用 Phoskia；`MotionBones` uniform block 直接声明 `bones[128]` 与 `previousBones[128]`，不依赖 raw `.sc` 文本反射。两条生产源均由 Windows `s_5_0` 编译测试覆盖，D3D11/D3D12 共用标准 Phoskia 编译与缓存路径。

## 7. LUT ColorGrading 契约

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

## 8. Present 契约

Present cache key：`present_final_ldr_blit_v1`。

Present 是最小的一纹理 fullscreen copy：

- 输入只来自 `FgSemantic::PresentSource`；
- 要求 `PresentSource` 当前指向的 logical resource 本帧 production latch 为真；
- 输出固定为默认 backbuffer；
- 不做曝光、tone-map、gamma、调色或 UI 合成；
- adapter 未初始化、Noop backend、零 viewport、semantic/attachment 无效时返回 0；
- program、binding 或 fullscreen geometry 不可用时 fail-close 并保留诊断日志。

PostProcess、TAA、FXAA、SMAA、ColorGrading 与 Present 共用 `FullscreenPassGeometry` 的超大三角形创建、绑定、销毁及 view 配置逻辑，但各 Pass 仍独立拥有自己的 GPU buffer handle，生命周期不会隐式耦合。

## 9. View ID 保留表

| View | Owner |
|---:|---|
| 3 | Deferred MotionVector → RG16F velocity；Forward 下复用为 Transparent（两条管线互斥） |
| 4 | ColorGrading → ColorGradedColor（显式排在 FXAA 与 Present 之间） |
| 5 | TAA → persistent TaaColor（显式排在 PostProcess 与空间 AA 之前） |
| 15 | PostProcess → FinalLdrColor |
| 16 | Present → backbuffer |
| 17 | FXAA → FxaaColor（显式排在 Present 之前） |
| 18–25 | Shadow atlas slots |
| 26–30 | Bloom pyramid additional stages |
| 31 | Auto exposure adaptation |
| 32–242 | UI offscreen layer/RenderTarget（每帧最多 211 次 retained repaint） |
| 243 | GPU 粒子计算，显式排在 2D 合成 view 246 前 |
| 244 | 未抖动的编辑器选择轮廓 silhouette mask |
| 245 | TAA 诊断覆盖层 |
| 246 | 相机覆层 2D 合成（独立正交相机，位于 3D Transparent 之后） |
| 247 | SMAA edge detection → SmaaEdges |
| 248 | SMAA blend weights → SmaaBlendWeights |
| 249 | SMAA neighborhood blending → SmaaColor |
| 250 | GBufferDebug |
| 251 | Editor orientation axis |
| 252 | EditorOverlay 兼容保留 |
| 253 | Selection visibility mask |
| 254 | Selection screen-space composite（Present 后、EditorOverlay 前） |
| 255 | UI main composite |

`RenderViewOrder` 显式保证生产依赖顺序；新增 Pass 不得仅依据数字大小猜测执行次序，也不得复用上述区间。

## 10. 后续 Pass 的接入方式

后 tone-map、低动态范围效果应插在 PostProcess 与 Present 之间：

```text
FinalLdrColor → TAA/FXAA/SMAA → ColorGrading → OtherLdrEffect → NewLdrColor
                                                       └→ PresentSource
```

每个节点需要：

1. append-only 新增 `FgResourceId` 与需要的 semantic；
2. 在 FrameGraph 声明 read/write 和目标格式；
3. 成功 submit 后发布 current-frame production latch；
4. 仅在完整生产链有效时提升 `PresentSource`；
5. 把 `RenderPassSlot` append 到 ABI 尾部，并在默认/Deferred 管线中明确位置；
6. 增加生产 shader 编译、view 唯一性、Noop、resize、teardown 与 stale-handle 测试。

运动模糊可以复用当前 opaque/cutout/transparent motion vector 与 reactive 标志，但在成为产品效果前仍需定义采样方向/长度门限、相机 cut、速度 tile-max/neighbor-max、遮挡边界及透明采样权重；TAA 的 3×3 有效速度 dilation 不是完整 Motion Blur 速度层级。

## 11. 验收

- `Test_PresentPass`：ABI、管线顺序、view 区间、current-frame latch、Noop、运行时 shader 编译。
- `Test_FXAA`：append-only ABI、默认/Deferred/Editor 顺序、运行时开关、失败回退、对比度门控、水平/垂直边缘分类、1/2/4/8 搜索上界、边缘 clamp、质量参数及 D3D11/D3D12 `s_5_0` 生产 shader 编译。
- `Test_SMAA`：append-only ABI、三阶段管线/view 顺序、FXAA 互斥、完整程序化 Area/Search lookup、官方 SearchTex 哈希、diagonal Area 参考抽样、45° 阶梯交叉边非零权重、semantic 事务提升、Noop/双重销毁、High shader 契约及三个像素阶段的 D3D11/D3D12 `s_5_0` 编译。
- `Test_TAA`：append-only ABI、Deferred-only 管线/view 顺序、三种 AA 互斥、Halton 序列、投影 jitter、Noop/双重销毁、重投影/YCoCg/邻域限幅契约及 Phoskia 生产源的 Windows `s_5_0` 编译。
- `Test_MotionVector`：append-only ABI、RG16F/view/Deferred 顺序、稳定 ID、连续帧 object+mesh+pose 历史、哨兵、opaque/cutout/骨骼 shader 契约、Noop/双重销毁及两条 Phoskia 生产源的 Windows `s_5_0` 编译。
- `Test_ColorGrading`：append-only ABI、管线/view 顺序、参数持久化、LUT 布局、semantic 成功后提升、Noop/双重销毁、生产 shader 编译与 binding 反射。
- `Test_FinalPP_S1c`、`Test_PostProcess_F5`、Bloom/DepthHaze/SSAO 套件：上游合成与来源选择回归。
- D3D11/D3D12 真机：确认 TAA/FXAA/SMAA/off 的轮廓与清晰度差异；TAA 还需覆盖静止画面收敛、相机平移/旋转、camera cut、resize、动态角色拖影和 Gizmo 稳定性。并检查 ColorGrading 四预设/强度、画面方向、Editor viewport rect、Bloom 开关、Debug/EditorOverlay/UI 遮挡顺序。

## 12. 当前未实现

- 精确 sRGB OETF、dithering 与 HDR swapchain 输出；
- 外部 `.cube`/图片 LUT 导入、SMAA edge/weight 与 Motion Vector 调试视图；
- DOF、MotionBlur、WorldLit2D velocity、透明多层深度/折射 reactive 与完整时间域重投影；
- 基于 GPU capture 的带宽与 RenderTarget alias 优化。
