# AYRenderer Design

## 2026-09-23 — 基础画质第四刀：Bloom 金字塔与自动曝光

- Bloom 从单层半分辨率横/纵模糊升级为 1/2 → 1/4 → 1/8 → 1/16 的能量守恒降采样金字塔，再以 tent filter 逐级上采样并与较高分辨率层合并；最终仍发布兼容的 `BloomBlurB`，PostProcess 和外部调用点不需要改写。
- FrameGraph 新增五个金字塔中间资源和 1/8、1/16 extent，完整 Bloom 现在是 Extract + 7 个有序 fullscreen draw。Bloom 关闭时整条分支仍由图存活分析裁剪，目标不会分配，也不会留下 stale producer。
- 自动曝光是独立、默认关闭的 `AutoExposurePass`：从当前 HDR scene 取固定 4×4 对数亮度样本，输出 1×1 RGBA16F 曝光历史，按变亮/变暗速度做帧率无关适应，并在 PostProcess tone-map 前与手动 Exposure 相乘。关闭时不创建历史 FBO、不增加 draw；resize、效果关闭和生命周期重建会使历史安全失效。
- RenderSettings 增加 `Auto Exposure` 开关并持久化；手动 Exposure 保留为曝光补偿。生产 Bloom down/up、AutoExposure 与 PostProcess shader 均通过 Phoskia/shaderc 编译，FrameGraph 完整链、资源契约和编辑器面板均有定向覆盖。
- VS 2026 Insider Debug：`AYRenderer_Test` **5374/5374**，`AYEditor_Shell` **1184/1184**；`AYEditorShell_Demo` 链接通过。选择轮廓仍保持 Post-TAA，下一项画质收尾是对其稳定 mask 做局部覆盖率抗锯齿。

## 2026-09-23 — 基础画质第三刀：IBL v2 split-sum

- 立方体环境资源保留只读 RGBA8 CPU 面数据；资源 id 变化时一次性生成 cosine-weighted diffuse irradiance cube、GGX prefiltered specular mip chain 和 split-sum BRDF LUT。正常帧只采样生成纹理，不运行卷积 Pass，也不增加 FrameGraph 节点。
- Deferred Lighting 与内建 Transparent PBR 共享同一组 IBL 资源：漫反射使用 irradiance，镜面反射按 roughness 选择显式 mip，并用 BRDF LUT 修正 Fresnel/geometry 项。旧 `envCube` ambient 仍作为预计算不可用时的回退，生成失败对同一资源只记录一次，避免逐帧重算卡顿。
- BGFXAdapter 增加 mutable mipmapped cube 创建与 face/mip 更新；AYShader 增加 `texturecube sampleLod` 生产转换。CPU 常色立方体测试固定卷积、mip 和 LUT 输出，Lighting 与 PBR 已通过 Phoskia IR 及 Windows `s_5_0` 生产编译。
- 当前生成纹理使用 RGBA8，预计算发生在环境首次进入 Lighting 的帧；后续可迁移为离线 cook/RGBA16F 以提高 HDR 范围并完全消除首次生成成本。下一刀为 Bloom 多尺度金字塔与自动曝光。

## 2026-09-23 — 基础画质第二刀：Directional CSM 与 Spot 透视阴影

- Shadow atlas 保持八槽兼容布局：先为每盏受支持的投影阴影灯保留一个 base tile，再把剩余槽位分配给第一盏 Directional 的 near/mid cascade；原 scene-fit Directional tile 作为 far fallback。`perLightShadowCount` 与总 projection count 分离，避免旧透明/自定义 receiver 把 cascade 误认成额外光源。
- Directional CSM 使用三段 practical split（lambda=0.65），相机视锥切片用稳定正方形包围，半径量化并将光空间中心对齐到 atlas tile texel。Deferred Lighting 依据相机前向深度选择 near/mid/far；Spot 改用光源位置、外锥角和 range 的真实透视投影。
- Forward、内建透明 PBR 与旧自定义材质继续采每灯 base tile，画面语义不回归，但暂不享受 near/mid cascade 分辨率。Point omni 仍明确不支持。下一阶段为 IBL v2（irradiance、prefiltered specular、BRDF LUT）。
- 私有 `ShadowPass` 新字段改为尾部追加，避免 VS 增量构建混用旧构造代码时移动既有字段偏移；本轮定向重编直接包含该头的 Renderer/Test TU，未清理整个引擎。

## 2026-09-23 — 基础画质第一阶段：TAA velocity dilation 与透明 reactive motion

- MotionVectorPass 继续使用单个全分辨率 RGBA16F 目标，不新增常驻 reactive RT。A 通道的原 `valid + previous-depth footprint` 保留在 `[0,2)`，透明表面追加 `+2` reactive 标志；新出现的透明对象可写 reactive 而保持 velocity 无效，避免误用伪零速度。
- Opaque/Cutout 后按 TransparentPass 相同的稳定远到近顺序重放透明几何；借用 GBuffer depth、关闭 depth write，使不透明前景仍能遮挡透明 motion，而最近可见透明层最终覆盖 velocity。透明重放复用刚体/蒙皮对象历史和材质 albedo/opacity coverage，不为透明路径建立第二套历史缓存。
- TAA 的 3×3 dilation 改为选择“最近且 velocity 有效”的样本， invalid 近邻不再挡住可用运动；velocity、previous depth 与 footprint 始终从同一 texel 读取。当前像素若带 reactive 标志，则使用透明对象 velocity 重投影、跳过不匹配的 opaque depth 层验证，并把 temporal feedback 限制为正常值的 15%，以抑制透明拖影而不完全退化为单帧锯齿。
- Motion debug overlay 将 reactive 像素显示为橙色，B 仍表示解包后的 velocity validity。Phoskia cache key 升级到 Motion v7 / TAA v11 / GBufferDebug v5；D3D11/D3D12 shader 生产编译定向回归通过。

## 2026-09-23 — Selection Outline 稳定化与 R6-6 收口

- 橙色 Selection Outline 不再依赖“jittered 遮罩 + 合成时反向偏移”消除抖动。view 244 以未 jitter 投影、无深度只写 Alpha 稳定轮廓；view 253 以场景同一 jitter 投影和 GBuffer depth 只写 RGB 可见性；view 254 在 Present 后分别按稳定/对齐 UV 读取两个通道并合成。这样固定输出像素上的边界覆盖率不再随 Halton 相位变化，遮挡判定仍与当帧深度一致。selection target 在 texture 或任一 borrowed FBO 失效时整体重建，覆盖 resize/backend generation 边界。
- RenderDoc 1.46 最终序列得到 24 个彼此独立的 D3D11 capture，覆盖 selection、resize 首帧/稳定帧、camera cut、效果关闭/重开首帧/稳定帧、透明边界静止/小幅移动、10 个 raw resource channel、5 个 TAA diagnostic mode 和双投影阴影 atlas。异步 `TriggerCapture` 请求间隔扩大到五帧，Motion 与 SSAO 不再发生文件名/通道串帧。
- 效果关闭帧中 Motion、SSAO、Haze、Bloom 与 TAA 对应 view 均消失；重开首帧即恢复完整链且未读取 stale target。resize 首帧未混用旧、新尺寸资源；双阴影灯捕获同时出现 view 18/19 和两个 atlas 区域。透明边界最终画面完整，但 `TAA-TRANSPARENCY-01` 仍作为无透明 depth/motion 对应面的已知能力限制保留。
- 新链接的 `AYEditorShell_Demo --renderer d3d12` 连续运行 15 秒未退出，日志记录 renderer type 3 与主链首帧提交。presentation bootstrap 失败路径现会关闭 splash、写入持久日志、显示模态错误并安全 shutdown，不再静默闪退。
- `AYRenderer_Test` 全量 **4698/4698**，`AYEditorShell_Demo` 链接通过。R6-6 完成；完整证据见 [`docs/d3d11-capture-report-2026-09-23.md`](docs/d3d11-capture-report-2026-09-23.md)。

## 2026-09-21 — R6-6 D3D11 基线抓帧（部分门禁通过）

- 使用 RenderDoc 1.46 对 Renderer `dd6599d` / root `9ab3998` 的 `AYEditorShell_Demo --renderer d3d11` 连续捕获 frame 24490、24663、24686。最后一帧窗口为 1536×912、编辑视口为 1083×594、swapchain 为 RGBA8 4×MSAA；捕获可解码并导出结构化 XML，最终缩略图完整。
- GPU 事件顺序为 Shadow atlas clear/caster → Skybox → GBuffer → MotionVector → SSAO → Lighting → DepthHaze → Transparent → selection mask → Bloom extract/H/V → PostProcess → TAA → Present → selection composite/editor overlay/UI。主链未发现 producer/consumer 倒序。
- 实际格式与契约一致：四 MRT 为 RGBA8、RGBA8、RGBA16F、RGBA8，Depth 为 R24G8 typeless/D24S8；Motion RGBA16F、SSAO RGBA8、Lighting/Haze/Bloom RGBA16F、FinalLdr RGBA8、TAA history RGBA16F。
- 三帧直接证明 TAA 双 history 正确 ping-pong：24490 写 3632/读 3636，24663 写 3636/读 3632，24686 写 3632/读 3636；Present 每帧采样刚写完的 history target，没有 history 自读自写。
- 本次只关闭正常主链的顺序、格式、绑定和静态 TAA ping-pong。resize、camera cut、效果关闭/重开、diagnostic channel、raw texture 像素内容和多灯 Shadow 仍需后续 capture，R6-6 总门禁保持开放。证据与逐 Pass 表见 [`docs/d3d11-capture-report-2026-09-21.md`](docs/d3d11-capture-report-2026-09-21.md)。
- bgfx 会把 RenderDoc 热键覆盖为 F11、关闭 in-app overlay，并把输出模板覆盖为工作目录下 `temp/bgfx`；F12 无响应是既定集成行为，不是注入失败。

## 2026-09-21 — R6-6 第二刀：延迟纹理调试入口

- 复用现有 view 250 的 `GBufferDebugPass`，保留 0–5 的 GBuffer 通道数值并 append-only 增加 6 MotionVectors、7 SSAO Occlusion、8 TAA History、9 Shadow Atlas；公开 byte channel API 不变，旧宿主不会发生枚举重排。
- 新通道只绑定一个由 CPU 按当前选择解析的 `auxiliary` 纹理，不为四类诊断长期占用四个 sampler；默认关闭时不提交 draw、不创建 RT，也不改变 Deferred 主链和 Present 顺序。
- MotionVector 与 SSAO 必须来自资源黑板的当帧已产出项；TAA History 必须具有有效 persistent content；Shadow Atlas 必须由 ShadowPass 明确报告可采样。缺少所选资源时 fail-close，不显示旧帧或任意 fallback 内容。
- Motion 以 RG 映射有符号速度、B 显示有效性；SSAO 和 Shadow Atlas 使用灰度；TAA History 显示当前 history-read 颜色。TAA resolve/rejection/weight/clipping/reprojected-history 继续由既有 view 245 diagnostics 提供，避免重复实现第二套 resolve 调试。
- Phoskia cache key 升至 `gbufferdebug_v4_r6_diagnostic_textures`；定向测试 58/58、全量 4694/4694，包含 Windows D3D11 `s_5_0` 生产编译，`AYEditorShell_Demo` 链接通过。Noop 的合法空后处理图保持 declared/live 0/0。真正的 D3D11 帧捕获仍按 [`docs/d3d11-capture-checklist.md`](docs/d3d11-capture-checklist.md) 执行，当前代码与单测不替代画面验收。

## 2026-09-21 — R6-6 第一刀：运行时架构诊断

- 公开 `RenderFrameStats` 以 append-only 方式增加 `graph` 与 `resources` 两组 backend-neutral 摘要；不暴露 bgfx handle 或内部 FrameGraph 类型。现有每 Pass CPU/GPU 时间、draw count 和既有字段顺序保持不变。
- FrameGraph 每帧报告 compile 成败/错误数、declared/live pass、logical resource、当前与进程内峰值 transient target，以及 retained/idle/released target。峰值由 `DebugOverlay::resetStats()` 显式归零，便于一段 capture 会话单独测量。
- 资源黑板每帧报告 declared、available、content-valid、当帧 produced、invalid 与 persistent-history 数量；统计在所有 Pass 和 TAA `finishFrame()` 完成后采样，因此反映最终生产状态，不参与渲染门禁。
- 现有 bgfx debug text overlay 增加 `FG` 与 `BB` 两行，compile failure 使用错误色显示。关闭 overlay 时仍可通过 `Renderer::getFrameStats()` 获取同一摘要，不增加 draw call 或 GPU target。
- 定向测试覆盖黑板计数、峰值保持/重置和 Renderer 实际填充；全量 `AYRenderer_Test` 4677/4677，`AYEditorShell_Demo` 链接通过。
- 下一刀补齐 MotionVector/SSAO/Shadow atlas 独立调试视图并整理 D3D11 capture 清单；本刀没有把单元测试或截图等同于真实 GPU capture 验收。

## 2026-09-21 — R6-4 资源黑板第三刀（logical output 迁移完成）

- LightingColor 与 SkyboxColor 作为 pass-owned `External` 输出发布，带尺寸与 generation。SceneColor routing、Transparent 与 Lighting backdrop 的生产路径不再用具体 Pass 的 `producedThisFrame()` 判断纹理新鲜度。
- BloomExtract 发布 `BloomBright`，BloomBlur 从黑板读取 bright 并发布 `BloomBlurA/B`，PostProcess 从黑板读取最终 blur texture。成功 submit 后同时设置 FrameGraph production latch，使图状态和执行状态不再分裂。
- SkyboxPass 指针仍用于 cube texture 资源 ID，LightingPass 指针仍用于 transparent ambient strength，ShadowPass 指针仍承载 atlas/矩阵/bias 元数据。这些不是可由单一 logical texture 表达的生产者输出，本刀不强行塞入黑板。
- `bloomExtractPass/bloomBlurPass/ssaoPass/depthHazePass` 等旧中部字段不删除，以保护旧 `PassExecContext` brace initializer 映射；生产路径在黑板存在时严格 fail-close，不会悄悄回退到旧 latch。
- 新增场景颜色/Bloom 跨帧失效和 lifecycle invalidation 测试；全量 `AYRenderer_Test` 4663/4663 通过。R6-4 完成，下一阶段是 R6-6 诊断与 D3D11 capture 门禁。

## 2026-09-21 — R6-4 资源黑板第二刀

- 黑板扩展到 GBuffer 五附件、SSAO occlusion 和 DepthHaze color。GBuffer 使用原子资源视图：任一附件未在当帧产出，或 FBO、尺寸、generation 不一致，整组就 fail-close，防止 resize/重建边界混用附件。
- GBufferPass 在成功完成 MRT clear/draw 后发布五附件；SSAOPass 与 DepthHazePass 在成功 submit 后发布 FrameGraph-owned `Transient` 输出，并补齐 FrameGraph 的 current-frame production latch。
- Lighting、MotionVector、TAA、SSAO、DepthHaze、Transparent、SceneColor routing 和 GBufferDebug 的生产路径已优先使用黑板。旧 `gbufferPass/ssaoPass/depthHazePass` 字段暂不从 `PassExecContext` 中删除：它们位于聚合中部，直接删除会改变大量旧 brace initializer 的字段映射；空黑板下仅作直接单测兼容。
- pipeline rebuild、resize 和 backend reset 会失效所有已登记当帧输出；每帧边界自动清除 External/Transient 的内容有效性。新增原子组、混合 generation 拒绝与 transient 跨帧失效测试，全量 4638/4638 通过。
- GPU 句柄所有权仍未迁移：GBufferPass 继续拥有 MRT，FrameGraph 继续拥有 SSAO/Haze target。下一刀候选是 Lighting/Skybox/Bloom logical output；Shadow 的 atlas 和光源矩阵元数据单独处理。

## 2026-09-21 — R6-4 资源黑板第一刀

- 新增 renderer 内部 `RenderResourceBlackboard`，首批覆盖 MotionVector 与 TAA read/write history。黑板为每个资源记录 borrowed framebuffer/texture、尺寸、generation、`External/Transient/PersistentHistory` lifetime、当帧产出状态与内容有效性。
- TAA 生产路径通过黑板读取 history 和 motion，FrameGraph plan 的对应 imported handle 也改由黑板解析。`PassExecContext` 尾部增加可空黑板指针，保留直接 Pass 单测的兼容路径，避免再次引入聚合初始化 ABI 风险。
- resize、camera cut、pipeline rebuild、backend reset、feature disable、资源重建、prepare/producer failure 和 shutdown 均使用显式失效原因；MotionVector 的当帧内容不再跨帧沿用，TAA history 则保留到显式失效。
- 本刀不迁移 GPU 句柄所有权：MotionVectorPass/TAAPass 仍负责创建和销毁，黑板只是状态与诊断的单一入口。后续再扩展 GBuffer/SSAO/DepthHaze 等 logical output，不把对象骨骼历史缓存强行并入。
- 新增黑板生命期/失效原因单测，并覆盖 TAA resolve 失败后以 `ProducerFailure` 联动失效。

## 2026-09-21 — TAA 诊断层纹与透明边缘异常观察（开放风险，暂缓处理）

**跟踪项：TAA-TRANSPARENCY-01。状态：基础 motion/reactive 修复已落地，多层透明与折射仍开放。** 本节保留 2026-09-21 的视觉观察；2026-09-23 已增加可见透明几何的 velocity、reactive 标志和低反馈路径，但这不等同于拥有逐层透明深度或折射专用时域解法。

### 用户观察与复查入口

- 场景包含透明立方体、不透明地面和天空盒。开启 TAA，在 RenderSettings 的 TAA Diagnostics 中检查 Clipping difference，并与相同场景/机位的 Final image 对照。
- **静止时 Clipping difference 全黑**；此前“静止全白”的描述已由用户纠正，不作为问题事实。
- 移动相机时物体稍微变亮、出现轮廓，以及部分较明显的层纹；大幅移动时部分区域更亮。
- 透明立方体边缘出现明显的白色不规则边框，边框在后方地面处呈现被裁断的观感。这里只记录**诊断画面的视觉关系**，尚未确认实际透明几何存在遮挡错误。
- 用户确认 **Final image 干净**；此前 History 调试画面仍有交替闪烁，但最终画面闪烁已几乎不可见。不得把两种诊断现象直接等同于最终画面仍有突变。
- 本条来自用户视觉反馈与代码核对，尚未取得对应机位的逐帧 GPU 捕获、数值读回和精确复现场景；本次观察的后端、窗口尺寸、透明材质参数及移动幅度未单独记录，后续复查须补齐。

### 已知实现事实与尚待验证的解释

- Clipping difference 输出 `clamp(length(historyBeforeClip - historyAfterClip) * 8, 0, 1)`，比较发生在 YCoCg 空间。亮色表示较强裁剪，不是错误标记；黑色既可能是无需裁剪，也可能是历史无效而跳过计算，必须结合 History rejection / History weight 判断。
- 当前颜色在 TAA 前已经合成透明物体，Transparent 的表面绘制仍只读不透明深度且不写深度；MotionVectorPass 现会按相同排序重放可见透明几何，并在 A 通道携带 reactive 标志。TAA 对该像素使用透明对象 velocity、跳过不匹配的 opaque depth 层验证并降低反馈。代码入口见 [TransparentPass](src/detail/TransparentPass.cpp)、[MotionVectorPass](src/detail/MotionVectorPass.cpp)、[TAAPass](src/detail/TAAPass.cpp)。
- 因此，“透明颜色完全只能沿用后方地面的运动”已被基础版修复；仍存在的对应局限是单个 motion texel 只描述最终可见透明层、没有透明自身深度，也没有覆盖折射后背景或多层透明。它们仍可能在复杂透明边界放大裁剪差异，但不再作为普通单层透明的默认解释。
- 普通物体的层纹另行保留待查，不能直接归因于透明、深度精度或阴影 bias；此前通过 shadow bias 消除的自阴影层纹，不足以证明本条层纹来自同一原因。
- 现有不透明/Cutout 与透明基础 motion/reactive 的 shader/契约回归通过；仍未覆盖真实 GPU 下的多层透明深度/运动一致性。未来折射、透明层相对运动、背景对比度或历史反馈策略变化，可能暴露拖影、边缘缺口或闪烁；这些是风险，不是当前已观察到的 Final image 缺陷。

### 后续重启条件与关闭标准

- 当 Final image 出现上述伪影，或开始扩展透明运动、折射、多层透明及其他时域效果时，重新打开专项查证；不因本条记录立即扩展架构。
- 在同机位配对捕获 Final、Clipping difference、History rejection/weight、Motion、当前/历史颜色及 GBuffer depth/coverage；覆盖静止收敛、小幅与大幅相机移动、透明物体独立运动、地面/天空交界，并对照隐藏透明物体及改变透明度的结果。记录每帧实际诊断模式、输入、有效历史贡献和裁剪前后数值，区分输出显示问题、颜色差异与表面匹配问题。
- 保留一个包含真实透明合成的最小回归场景，在 D3D11/D3D12 验证。关闭本条须有可复现的原因解释、必要修复/明确适用边界和最终画面验收；**不以“所有诊断图必须全黑”为标准，也不以降低显示倍率、放宽裁剪/拒绝阈值或仅凭 Final 暂时干净掩盖未解释的风险。**

## 2026-09-21 — 架构审核已暴露问题修复（不扩展架构）

- **可选依赖顺序**：契约校验预扫描挂载输出；可选生产者可以缺席，但已挂载时必须先于消费者执行。错误使用 `optional-producer-after-consumer` 定位资源，保持拒绝错误描述且不拆除原管线。覆盖 Present→FXAA、ColorGrading→SMAA、Lighting→SSAO/Skybox、TAA→Motion 的倒序组合，以及缺席/正确排序的合法配置。
- **Skybox 本帧产出**：与 GBuffer/SSAO 一致，帧头、execute 入口及销毁清除 production latch，仅成功 submit 后置位。Lighting 同时检查 enabled、本帧产出和有效附件；失败时绑定已验证的 RGBA8 fallback 并上传零 skyMix，不再假定未绑定 sampler 为黑色，也不复用旧天空。新增 `AYRenderer_SkyOutputGpu`，实际 Skybox→空场景 GBuffer→Lighting 验证有效、禁用、恢复、inactive source、丢失纹理和销毁；D3D11/D3D12 各 **50/50**。
- **GPU 统计**：先复制 CPU pass counters，再合并最新完成 GPU 帧的计时，避免整体赋值覆盖 GPU 数值；纠正 Present/FXAA/Motion/TAA/ColorGrading/SMAA、阴影 atlas、透明选中绘制和 EditorOverlay 的 View 归属。CPU 当前提交和 GPU 最近完成帧仍不是同一时间点，保留 `gpuFrameNumber` 区分。本轮不实现完整历史帧统计关联。
- **Phoskia**：通用左结合修复落在 AYShader；TAA/Motion 先前的显式括号与分步表达式保留。编译缓存 schema 升至 `aybgfx-v4-left-associative`。真实 GBuffer 验证另暴露 raw-SC 编译入口先默认构造 driver 的异常问题，已在 AYShader 修正为保护内按资源池路径直接构造；没有新增绕开 Phoskia 的生产 shader。
- **验证**：VS 2026 Insider x64 Debug 构建 Renderer/Shader/Editor Demo。Renderer 常规 **4556/4556**，Shader **1466/1466**。D3D11/D3D12 独立 GPU 各 **6255/6255**：Sky 50、TAA 固定网格 178、有效支持权重 682、Motion 772、常驻历史 163、拒绝联合查证 1004、边缘递归 3406。GPU suite 必须独立进程设置 `AY_TAA_GPU_TEST=d3d11` 或 `d3d12` 后执行同名 suite；常规 Noop 总数不算 GPU 验证。
- **用户视觉反馈**：History 调试画面仍在两幅画面间交替闪烁，但最终画面已几乎不可见闪烁。这是此前 TAA 修复的用户反馈，不能表述为调试闪烁完全消除，也不能当作本轮架构问题修复的视觉验收。
- **后续收口**：本节当时延后的 FrameGraph 执行门禁、统一 Pass 结果状态、共享 DrawList 与闲置 RT 回收，已由下节的架构审核 5/6/7 完成；自动 alias 和完整资源黑板仍未实现。

## 2026-09-21 — 架构审核 5/6/7 收口

- 编译后的 FrameGraph liveness 已成为带 slot Pass 的实际 dispatch 门禁；TAA 图依赖补齐 GBuffer、MotionVector 和双 history。它仍保留既有 Pass 注册顺序，不等同于自动调度器。
- FrameGraph owned target 连续 120 帧 inactive 后释放 pool lease，避免关闭效果后永久保留 RT；再次启用时按正常路径重新申请。
- `FrameDrawLists` 每帧单次扫描并生成 opaque、GBuffer、transparent、WorldLit2D、Overlay2D、shadow caster/bounds 与 selection outline 列表。GBuffer、MotionVector、ForwardOpaque、Forward2DOpaque、Transparent、Shadow 均已迁移；透明与 Overlay 保持稳定排序，仍逐对象 submit。
- `RenderPipeline` 内部执行结果明确区分提交成功、正常零绘制、禁用、图裁剪和失败，消除 `execute()==0` 的语义混叠；公开 ABI 未增加字段。
- 本刀不启用 frustum cull：蒙皮、透明描边和阴影参与者需要先有统一可靠 bounds；也未实现 instancing、自动 RT alias 或 R6-4 资源黑板。
- VS 2026 Insider x64 Debug 完整重编后，`AYRenderer_Test` 全量 **4603/4603**；共享 DrawList、Pass outcome、FrameGraph retention/liveness 与阴影矩阵均有定向覆盖。真实 D3D11 画面和 CPU/GPU capture 仍归 R6-6，不以 Noop 单测替代视觉验收。

## 2026-09-21 — TAA v10：按有效历史贡献连续降权

- 已修复前一轮查证的极小历史 tap 放大：四 tap 深度筛选后的权重和保留为 `historyConfidence=clamp(historyWeight,0,1)`，最终历史反馈乘以该可信度。RGB 仍归一化重建颜色，但不再把归一化误当作完整可信度。保留原 `0.00001` 数值安全门限，不引入实验性的 0.5 硬拒绝阈值，不修改深度容差、jitter、静态/运动基础反馈或颜色裁剪。
- 新增 `AYRenderer_TAASupportGpu`：RGBA32F 与生产 RGBA16F 两档，正负亚像素移动、极小剩余贡献、0.5 两侧连续性、完整有效历史下的 2.25px 移动，以及全部深度不匹配的遮挡回退。实际 GPU 像素验证反馈为 `baseFeedback * survivingSupport`；D3D11/D3D12 各 **682/682**。
- `AYRenderer_TAARejectionGpu` 默认改为 127×127，并把已知错误断言改为生产版不再出现 >0.05 的拒绝跳变。保留仅测试的 `AY_TAA_PROBE_LEGACY_FEEDBACK=1` 对照，移除历史可信度乘项即可重新复现旧错误，不保留 0.5 实验逻辑。
- D3D11/D3D12 默认联合查证各 **1004/1004**；实体背景近景立方体的拒绝步长由 **0.425537 降至 0.0273438**，六组场景均无 >0.05 的拒绝跳变。D3D11 另复核 32×32、128×128，均没有明显拒绝跳变；不是专门对奇数尺寸打补丁。
- VS 2026 Insider x64 Debug 构建 Renderer 与 Editor Demo；Renderer 全量 **4486/4486**。两后端原边缘、固定网格、常驻 history、Motion GPU 分别为 **3406/3406、178/178、163/163、772/772**。Shader 继续走 Phoskia，缓存键升级 `taa_phoskia_supported_history_v10`，不复用旧二进制；无新增纹理、draw、采样或公开 ABI，仅增加可信度 clamp/乘法。
- 按用户授权关闭当前验证进程并更新安装包，用户资源/配置保留。真实 Editor 原机位仍由用户视觉验收；测试不宣称所有亚像素波动消失，也不覆盖透明多层运动、全部材质或宿主提交情况。完整原因与复现命令见 [查证记录](docs/taa-history-rejection-investigation.md)。

## 2026-09-21 — TAA 突变查证：极小历史 tap 被放大（尚未修复生产代码）

- 新增仅测试的生产 GBuffer/D24 → 借用深度 Motion → Resolve 联合查证。32×32 未复现明显突变；127×127 和 128×128 均复现，不能归因为“只有奇数分辨率有问题”。所测几何没有 Motion 漏写。
- 已捕获机制：深度筛选后仅剩约 `0.0000152` 的邻居权重，超过 `0.00001` 有效门限；RGB 归一化把它升为完整历史，feedback 仍为 0.92。前景颜色由此扩散到轮廓外，下一相位表面标签变化又拒绝历史，形成点状跳变。不是单纯“拒绝太严格”，不应继续统一放宽深度容差。
- 测试副本仅把有效 support 门限提高到 0.5 作因果对照：127×127 近景立方体/实体背景的拒绝步长在 D3D11/D3D12 均由 **0.425537 降至 0.0273438**；六组场景均无 >0.05 的拒绝跳变，正常积累波动保留。0.5 是实验值，**不是最终方案**。
- 完整证据、运行方法、测试覆盖边界及后续修复约束见 [TAA history rejection investigation](docs/taa-history-rejection-investigation.md)。本轮不改生产算法、缓存键、编辑器配置或安装包，尚须后续修复与用户同机位验收。

## 2026-09-21 — TAA v9 / Motion Vector v6：近景斜面历史深度误拒绝

- 用户确认 Motion/Clipping 静帧已稳定，但 History weight 的间断黑边对应 Final image 中实际可见的点状凸起和闪烁，近景更明显。本轮不将它归为纯诊断现象；不调整 jitter、反馈权重、颜色裁剪或阴影 bias。
- **可复现的缺陷**：3×3 最近表面选择随 jitter 相位取到同一斜面的不同位置；上一帧存储的深度与当前点的 previous-depth 有合理差异，固定 0.0025 阈值却反复拒绝历史。新增 GPU 斜面控制组（每像素 NDC 深度梯度 0.01）在关闭 footprint 补偿时拒绝 **323/640**，最后八帧红通道最大峰峰值 **0.749512**；启用后为 **4/640、0.103027**，与恒定深度控制组一致。此结果是合成输入的生产 Resolve 验证，不等同于已证明用户场景的全部剩余闪烁消失。
- Motion Vector RGBA16F 的 RG/B 语义不变；A 从 0/1 扩展为无效 0、有效 `1 + min(fwidth(previousNdcDepth), 1)`。导数在实际光栅化的同一图元上求得，位于分支及 cutout discard 之前，不从跨前后景的深度纹理估计坡度。TAA 以 `baseTolerance + 2 * footprint` 比较历史 tap；平坦表面维持原阈值。footprint ≥ 0.05 或 previous-depth 不在 [0,1] 时保守拒绝，避免近裁剪异常导数放宽到整个深度范围。它仍是有限容差的启发式，而不是表面身份匹配；非常接近的不同表面仍有混入历史的可能。
- history alpha 改存选中位置的真实 raster depth，避免从 RGBA16F world position 重建深度引入量化差异。两张 history 明确使用 point sampler；RGB 仍由 shader 对四个深度筛选后的 tap 手动双线性重建，不允许 alpha 在硬件过滤时混合天空和几何。保留原有 world-position 绑定和正 W 检查，不扩展公开 ABI、纹理格式或 RT 数量。
- 两个 Motion shader 缓存键同时升级 v6，TAA 升级 v9，全部继续经 Phoskia 编译；不改其全局解析语义。成本为 Motion 片元的深度导数及少量 ALU，没有新增全屏 pass 或纹理采样。
- **验证**：VS 2026 Insider x64 Debug 构建 Renderer 测试与 Editor Demo；Renderer 全量 **4484/4484**。独立 D3D11/D3D12：生产 Motion 光栅化各 **772/772**（增加斜面深度和 footprint 检查）；TAA 递归边缘各 **3406/3406**；GPU 常驻双缓冲各 **163/163**；原固定网格 Resolve 各 **178/178**。斜面用例还检查 0.03 的遮挡深度差、clip-range 外历史、极端 footprint、缺失 motion 的拒绝，以及 history alpha 的 raster-depth 来源。
- **验收边界**：Motion producer 的真实光栅化与 Resolve 的合成斜面测试分别覆盖，两者不是完整 Editor 场景端到端测试；常驻 history 回归也不替代真实宿主提交验收。短序列对照仍有约 0.103 的边缘覆盖波动，未宣称零闪烁。验证包同步更新，最终须在用户原近景机位检查 Final image、静止 History weight 及移动后的拖影/轮廓稳定性。

## 2026-09-21 — Motion Vector v5：去除假运动与 GPU 常驻历史验证

- 验证结果：VS 2026 Insider x64 Debug 构建；Renderer 全量 **4481/4481**、MotionVector 专项 **57/57**；旧 TAA GPU 固定网格/递归边缘测试在 D3D11/D3D12 仍分别为 **178/178**、**2604/2604**。Editor 验证程序已重新链接，生产缓存键 v5 强制重新编译修正后的运动 shader。
- 用户进一步确认静止画面呈固定 A/B 交替；另已通过阴影 bias 消除表面层纹，因此阴影自遮挡与剩余 temporal 闪烁分开记录。本轮不改 shadow bias、TAA jitter、历史权重、裁剪阈值或输出顺序。
- **已复现的错误**：Opaque/Cutout Motion Vector 均使用 `currentUv - previousUv - currentJitter + previousJitter`；Phoskia 的同级算术右结合将它解析成 `currentUv - (previousUv - (currentJitter + previousJitter))`。静止几何因而残留约 `2 * currentJitter` 的假运动。v8 的灰度/彩色 Resolve 测试上传理想零 velocity，无法覆盖生产运动着色器；这是上一轮的验证缺口。
- 两个变体均先分别计算 `unjitteredCurrentUv = currentUv - currentJitter`、`unjitteredPreviousUv = previousUv - previousJitter`，再相减。仍经 Phoskia 生成，缓存键同时升级 v5，不复用旧错误着色器。本轮局部明确表达式语义，**没有全局修复 Phoskia Parser 的结合性错误**，编译器修复仍应独立审计其他既有 shader 的行为变化。
- 新增 `AYRenderer_MotionVectorGpu`：隐藏测试窗口、实际生产 Opaque/Cutout shader 和真实三角形光栅化；透视/正交 × 刚体/骨骼 × 8 个 Halton 相位，分别检查静止零运动及 2px 的刚体/骨骼位移，同时验证上一帧深度和 valid。旧公式 D3D11 **128 个检查失败**，两种材质的静止/移动最大误差均为 **0.4375px**；修复后 D3D11/D3D12 均约 **0.000002861px**，各 **388/388** 通过。运动读回 RT 用 RGBA32F 隔离公式误差，不冒充真实 GBuffer 借用深度及完整场景提交验收。
- 新增 `AYRenderer_TAAResidentGpu`：两张 RGBA16F history 在 GPU 上逐帧交换，40 帧连续提交，每帧仅 blit 到独立捕获纹理，全部提交结束后才读回；历史从不读回 CPU 再上传，不插入逐帧读回等待。bootstrap 灰色 + 固定棋盘格邻域在黑像素应满足 `(191/255) * 0.92^N`，能检查读到 t-2、奇偶历史分裂、目标复用错误及意外重置。D3D11/D3D12 各 **163/163**，最大递推误差 **0.00124061**，低于 RGBA8 输入/RGBA16F 累积门限 0.004。
- 两项 GPU 验证均须独立进程设置 `AY_TAA_GPU_TEST=d3d11` 或 `d3d12` 后指定 suite；常规全量明确跳过。常驻历史测试使用生产 Resolve shader 和实际双缓冲，但不经过 Editor/TAAPass 的整条宿主提交，因此不能单凭通过就宣布用户所见的 A/B 闪烁已全部消失。最后仍需用户在同机位、静止、未选中状态验收 Final image 与 Motion 视图。

## 2026-09-21 — TAA v8：静止彩色轮廓闪烁、递归 GPU 回归与诊断

- 验证结果：VS 2026 Insider x64 Debug 构建；Renderer 全量 **4474/4474**、TAA 专项 **203/203**、Editor Shell **1180/1180**；独立 D3D11/D3D12 固定网格 GPU 读回各 **178/178**，新增递归彩色/硬边/运动/诊断 GPU 用例各 **2604/2604**。可执行程序与内置 UI 同步打包，视觉验收仍交由用户。
- 用户确认「相机静止且未选中仍闪烁」，因此本轮针对场景 Resolve，不归因于选中描边，也不以降低 jitter 或增大历史权重掩盖问题。静态/运动权重仍为 0.92/0.65，jitter spread 仍为 0.5。
- **确定的颜色错误**：实际 Phoskia 输出把 `resolved.x - resolved.y - resolved.z` 生成为 `resolved.x - (resolved.y - resolved.z)`，破坏 YCoCg→RGB 的蓝通道。灰度的 Co/Cg 为零，上一轮灰度用例无法发现。本轮显式写成 `(Y-Co)-Cg`，R 同样明确括号；增加生成源码括号断言和饱和彩色递归 GPU 保真检查。没有手写旁路 `.sc`。Phoskia 通用二元运算结合性问题仍需 AYShader 独立审计，本轮不全局改变语言解析语义。
- **统一 temporal surface**：在非 Reverse-Z 的 3×3 point depth 邻域选择最近表面，并从同一 UV 读取 coverage、world position、velocity/previous-depth/validity；同深度保持中心优先。不将前后景运动向量平均。新增 `sceneDepth` 绑定和必需的 GBufferDepth/GBufferSurface 契约；缺失输入继续 fail-close。
- **分离重建与裁剪**：输出颜色仍在固定网格用 `outputUV + currentJitterUV` 重建；裁剪盒改为原始 texel-center 的 3×3 YCoCg 范围，避免双线性过滤范围随相位缩小而截断细线历史。反应性降权依据历史超出裁剪盒的量，不再把盒内的正常覆盖率/色度变化当作突然变化。遮挡深度拒绝、缺失 velocity 拒绝和无历史回退保留。
- **验证方法**：新增独立 `AYRenderer_TAAEdgeGpu`，32×32 的竖边、约 22.5° 斜边、0.8px 细线，运行 96 帧；上一帧 GPU Resolve 输出作为下一帧历史，统计最后 32 帧中央区域 RGB 最大峰峰值。使用 RGBA8 当前颜色/coverage 和 RGBA16F 历史/运动输入；depth 由合成表面上传为浮点纹理，不是假装已覆盖真实 D24/几何光栅化。v7 D3D11 基线依次 **0.9375 / 0.91748 / 0.995605**，v8 D3D11/D3D12 均约 **0.04150 / 0.07422 / 0.08105**，门限 0.10。并检查红蓝覆盖的能量守恒、纯 RGB/混合色连续保真、8px 平移、遮挡消失、重置和全部诊断输出。
- **RenderSettings → TAA Diagnostics**：0 Final image；1 History rejection（红拒绝/绿接受）；2 History weight（灰度实际历史权重）；3 Clipping difference（YCoCg 裁剪距离×8）；4 Motion (pixels)（RG=xy/16+0.5，静止灰）；5 Reprojected history（深度筛选后、裁剪前颜色）。仅会话状态，不写入用户偏好；禁用 TAA 时不绘制。诊断模式不改变正常历史，独立的 view 245 在 Present 后、GBufferDebug/选择框/Gizmo/UI 前覆盖视口；同帧重绑全部输入和 uniform，不能依赖 submit 已丢弃的状态。切回 Final image 不必重置历史，重复设置相同 TAA enable 状态也不再重置。
- **成本与边界**：正常 Resolve 新增 9 次点深度读取和 1 次原始中心颜色读取，无新增历史 RT；调试开启才额外执行一次 Resolve 绘制，默认零额外调试 draw/RT（同一 shader 内保留诊断 uniform 分支）。静态契约同时修正 TaaColor 为 RGBA16F，并标明可选诊断 side effect。当前仍是 LDR TAA；没有声称消除所有亚像素波动、实现天空旋转重投影或透明/WorldLit2D 多层运动。2026-09-23 已补基础透明 velocity/reactive 与有效速度 dilation，WorldLit2D、多层透明和折射仍未覆盖。编辑器真实画面与真实 MotionVector 几何光栅化仍需视觉验收，合成 GPU 测试不能代替。
- 诊断的晚读目前依赖 FrameGraph 已有的 never-share RT 策略及 GBuffer/历史的整帧存活。将来启用瞬态 alias 前，必须把诊断 draw 的晚读范围纳入图的资源生命周期；不能仅按普通 TAA Resolve 的 view 5 作为 FinalLdr 的最后使用点。

## 2026-09-21 — TAA 固定输出网格与历史表面验证修正

- v7 明确区分原始 jitter 输入与固定输出/history 网格：当前颜色及几何输入在 `outputUv + currentJitterUv` 查询，历史在 `outputUv - unjitteredVelocity` 查询；3×3 YCoCg 邻域限制不再被当作当前颜色重建。当前重建以双线性为基线，后续锐利滤波另行验收。
- MotionVector v4 使用 `RGBA16F`：RG 为不含投影 jitter 的 Current−Previous UV，B 为上一帧同一刚体/骨骼表面的 NDC 深度，A 为历史有效标记。清屏、缺失身份、新对象和未覆盖的 draw 均无效，不再把缺失数据解释成静止。代价是速度目标从 4 增至 8 bytes/pixel；仅时间消费者启用时分配。
- 深度仍存放在 history alpha（天空为 −1），但按四个 texel 中心分别验证，再仅对有效 RGB tap 做双线性加权与归一化。禁止先把天空标记和几何深度插值后再比较；动态深度来自 MotionVector 的上一帧变形表面，不再以当前世界位置代替。
- 相机移动时保留 jitter，失效历史返回固定网格重建后的当前颜色；天空在相机运动时仍拒绝历史，尚未实现天空旋转重投影。准备阶段检查 GBuffer/Lighting/PostProcess/Present 均启用，帧末将失败的 TAA 与 Motion 历史成对失效，防止颜色和运动错帧。
- WorldLit2D 及其他未覆盖对象的完整运动重放尚未实现。WorldLit2D 几何缺少有效 velocity 时保守拒绝历史；不再使用对动态对象不可靠的 world-position 相机运动回退。2026-09-23 已补普通透明几何的基础 motion/reactive；透明叠加、折射和 SceneOverlay 的多层运动仍是独立后续工作。
- 新增可选 `AYRenderer_TAAGpu`：在独立进程设置 `AY_TAA_GPU_TEST=d3d11` 或 `d3d12`，创建隐藏测试窗口，实际编译生产 Phoskia Resolve 并读回 GPU 像素。覆盖 8 相位固定网格、动态前后深度、缺失 velocity、天空混合 tap、遮挡拒绝、无历史及越界。普通全量测试明确跳过该硬件用例，不能用普通测试数替代 GPU 验收。
- 本轮不迁移 HDR/Tonemap 顺序、不增加 Catmull–Rom、variance clipping 或速度膨胀；当前仍为 LDR TAA。Shader 缓存键升级，旧 v6/v3 二进制不复用。
- 验收边界：上述合成输入 GPU 测试不替代编辑器整场景视觉、真实几何 MotionVector 光栅化和右键 WASD 回归；编辑器移动闪退不能由 Shader 单测通过宣称已解决。
- 本轮结果：VS 2026 Insider x64 Debug 构建；TAA 190/190、MotionVector 50/50、Renderer 全量 4458/4458；独立 D3D11 与 D3D12 GPU Resolve 读回各 158/158。GPU 用例不依赖编辑器前台操作，不改变用户场景或配置。

> **2026-09-09 — WorldLit2D 第四刀（Shadow caster）**：ShadowCaster
> 不再粗暴跳过所有 `DrawPayload2D`；SceneOverlay 仍完全排除，
> WorldLit Sprite/Tilemap 则使用 alpha-mask caster 写入现有多光源
> shadow atlas。遮罩阶段与 GBuffer 共享 source rect、flip、tint alpha、
> baked-atlas UV 及 Nearest/Linear/4-tap/9-tap 采样语义，避免投出整张
> 卡片或错误图块。GPU mesh 上传时同步保留真实 bind-pose 局部
> bounds，Shadow 场景拟合不再假设所有网格都是单位立方体；
> Overlay 和 `ShadowFlags::None` 也不再污染光源视锥。

> **2026-09-09 — WorldLit2D 第三刀（Perspective visibility）**：Renderer
> 新增只读的未抖动主相机矩阵快照；未设置相机时返回无效，调用侧必须
> fail-open，不能把 identity 当视锥。`RendererSubSystem` 在初始化、FreeCam
> 更新、清除 override、视口比例变化及提交前同步相机，因此 ECS 场景构建阶段
> 可使用与当帧一致的 view/projection。此刀不改变 `RenderScene`/`DrawItem`
> 布局，也不把 TAA jitter 泄漏到 CPU 可见性判断。

> **2026-09-09 — WorldLit2D 第二刀（Tilemap）**：Tilemap chunk 可沿现有
> GBuffer 进入延迟光照，并通过 `UvMapping2D` 明确区分 Sprite 的 source-rect/
> bottom-left UV 与 chunk mesh 已烘焙的 atlas UV。`DrawPayload2D` 同时携带
> sampling quality；WorldLit albedo 保留 Nearest、Linear、4-tap、9-tap，
> normal/roughness/emissive 在中心 texel 采样，alpha cutout 使用过滤后的
> albedo alpha。默认 Overlay 路径与 view 246 行为不变。当前仍未覆盖 Blend、
> 2D 阴影、逐物体 Motion Vector，以及 3D 主相机下的 Tilemap chunk frustum
> streaming；第二刀曾对后者 fail-open，现已由上方第三刀关闭。

> **2026-09-09 — WorldLit2D 第一刀**：没有新增独立 Pass，而是在现有唯一
> GBuffer producer 内加入 Position+UV 专用的 2D geometry substage。新增
> `RenderDomain2D` 与 `Material2DDesc`；默认 `SceneOverlay` 保持原 view 246、
> 无深度 alpha 合成，显式 `WorldLit` 的 opaque/cutout Sprite 改由 3D 主相机
> 写入同一组 albedo/metallic、normal/roughness、world-position/AO/model、
> emissive/coverage MRT，因而复用现有 Lighting、SSAO、DepthHaze 与后处理链。
> 支持 albedo、normal、roughness、emissive、tint/atlas/flip，normal flip 会同步
> 修正切线空间方向；材质贴图区分 sRGB 与 linear 缓存。当前边界：只接
> Sprite，要求 Deferred；Blend、Tilemap WorldLit、2D 阴影投射和逐物体
> Motion Vector 留到后续刀。默认场景零行为变化。

> **2026-09-09 — R6 架构收口启动**：暂停新增独立画质 Pass，当前开工入口切换为 [`docs/render-architecture-r6.md`](docs/render-architecture-r6.md)。R6-1 已把 FrameGraph `compile()` 从“全部 enabled 即 live”升级为结构校验与反向存活分析：拒绝未声明 read/write、owned resource 先读后写、多写者及无生产者 semantic；存在 Present/Consumer 终端时从终端和 semantic 输出反向裁剪，兼容 SSAO 图外 Lighting 消费与 TAA imported history 本帧覆写。失败输出结构化诊断并让 owned 输出 fail-close。R6-2 新增不持有具体 Pass 的 `PostProcessGraphPlan`，集中声明 SSAO/Haze/Bloom/FinalLdr/AA/Grading/Present 资源链；`Renderer::render()` 只负责能力判断和计划输入，既有 `RenderPipeline` 执行顺序、view id 与画面拓扑未改变。R6-3a 已在 `RenderPipeline` 中加入随 `addPass/clear` 维护的 exact-type 索引，生产路径改用 O(1) 类型查找并移除字符串查找后的直接类型转换。R6-3b 已为 21 个 Pass slot 建立静态资源/输出/lifetime/side-effect 契约，管线重建前拒绝缺生产者、错序、重复或未知 slot；后处理 transient RT 由同一契约生成。下一刀 R6-3c 拆分 mounted/enabled/produced 状态。

> **2026-09-08 — 同一 Scene 的 2D / 3D 合成**：`RenderScene` 新增独立
> `OverlayCamera2D`，`OrthoCameraUpdateSystem` 不再覆盖 3D 主相机。
> `Forward2DOpaquePass` 使用专用 view 246，在 Sprite/Tilemap 全部 payload 间按
> `packedSortKey` 全局稳定排序，并于 3D Transparent 后写入当前 HDR scene color。
> Forward 与 Deferred 均挂载该 Pass，GBuffer/Transparent 显式排除 2D payload。
> 深度参与的 World Space 2D 留作独立域，详见
> [`ADR-0008`](../../AYDocs/adr/0008-mixed-2d-3d-scene-composition.md)。

> **2026-09-02 — 后处理 Phoskia 边界收口**：MotionVector 的 rigid/skinned 与 alpha-cutout 程序、TAA resolve 均从 raw bgfx `.sc` 迁回标准 Phoskia `ShaderResourcePool::acquire` 路径，恢复源码缓存与磁盘二进制缓存。为此 Phoskia BGFX 后端补齐真实 `if/else` 输出，并新增 fragment-only `discard`；TAA 改为单一最终输出，未把 material `return` 错当成早退。FXAA 与 SMAA 暂保留为受控 raw `.sc` 兼容例外：前者依赖用户函数与真正早退，后者依赖用户函数、C 风格循环和复杂嵌套分支；两者都有 Windows `s_5_0` 生产编译测试，不再作为新 Pass 绕过 Phoskia 的先例。

> **2026-09-01 — Deferred Motion Vector Pass**：新增 append-only `RenderPassSlot::MotionVector=20` 与 view 3，固定在 `GBuffer(view 7) → MotionVector(view 3) → SSAO(view 14) → Lighting(view 8)`。Pass 不扩展 GBuffer MRT：仅在 TAA 成功准备时分配独立全分辨率 `RG16F` velocity，并通过 non-owning FBO 借用 GBuffer depth，以 `LEQUAL` color-only 重放 opaque/cutout 几何。速度编码为 `currentUV - previousUV`；新对象、断帧或姿态布局变化写 `(2,2)` 哨兵拒绝旧 history。`DrawItem::motionObjectId` 负责稳定对象匹配，AYEntity 为刚体、分段蒙皮和回退路径提供混入 World/handle generation 的标识；Pass 按对象+mesh 保存上一帧 world 与完整骨骼姿态。TAA 优先消费速度，资源或 shader 不可用时保留 RT2 world-position 静态回退；透明物体仍不在本轮覆盖。

> **2026-09-01 — TAA 抖动与选中框隔离（历史实现，已由 2026-09-23 双栅格方案取代）**：Halton jitter spread 收敛到 0.75，静态/运动 history feedback 调整为 0.92/0.65，并让亮度 history rejection 扣除当前 3×3 邻域的正常边缘跨度，降低浅角斜边的覆盖闪烁。当时保留标准 Halton 顺序，view 253 的遮罩与场景深度使用同一 jitter 投影，view 254 在 Present 后反向对齐；后续视觉验收证明反向偏移不能消除光栅 coverage 变化，因此当前实现已改为 view 244 未 jitter Alpha silhouette 与 view 253 jittered/depth-tested RGB visibility 分离。选中框始终不进入 Bloom/PostProcess/TAA history，方向轴与 Gizmo 继续使用稳定覆盖层路径。

> **2026-09-01 — Deferred TAA 首版**：新增 append-only `RenderPassSlot::TAA=19`、`FgResourceId::TaaColor=12` 与 view 5，接入 `PostProcess → TAA → FXAA → SMAA → ColorGrading → Present`。TAA 使用 8 样本 Halton jitter、Pass-owned 双 `RGBA16F` history、GBuffer RT2 世界坐标重投影、RT3 coverage、稳定相机下的背景 jitter-delta 重投影、3×3 YCoCg 邻域限幅和运动/亮度自适应反馈；只有 shader、几何和 history 全部就绪才启用 jitter。TAA/FXAA/SMAA 在 Renderer 与 Editor Render Settings 中三选一，配置可持久化；Editor Gizmo 保持未 jitter 投影。首版只覆盖相机运动、静态 opaque 与静止背景轮廓；逐物体/骨骼限制已由上方 MotionVector 记录关闭，透明运动仍待后续契约。

> **2026-09-01 — FXAA/SMAA 色度边缘修复**：Editor 实机复查确认两个 Pass 的 FrameGraph 调度、互斥开关和最终输出链路均正常，视觉近似失效的共同原因是边缘阶段只依赖亮度；验证场景的高饱和橙色与灰绿色表面存在明显的等亮异色边界。SMAA Edge 已切换为官方 color-edge 判定（RGB 最大通道差），FXAA 在保留亮度梯度、方向搜索和克制的 0.50 sub-pixel correction 基础上加入色度范围、色度二阶导数与端点判定。新 cache key 为 `smaa1x_color_edge_v2` 与 `fxaa_chroma_edge_search_v3`；D3D11 Editor A/B、D3D11/D3D12 生产 shader 编译、`AYRenderer_FXAA` 63/63、`AYRenderer_SMAA` 105/105 与全量 3818/3818 均通过。

> **2026-09-01 — SMAA 1x High**：独立三阶段 LDR Pass 按 Edge Detection（view 247）→ Blend Weight（248）→ Neighborhood Blend（249）生产 `SmaaColor`，仅在完整提交后提升 `PresentSource`。Renderer 保持 FXAA 默认开启/SMAA 默认关闭，Editor 默认选择 SMAA，两个开关互斥。视觉复查先修正 linear/clamp 与 0.25 像素交叉边解码，随后升级到 High：16 步正交搜索、8 步 diagonal detection、SearchTex 长度校正和 25% 官方 corner detection。完整 `160×560 RG8` AreaTex 与 `64×16 R8` SearchTex 均按 MIT 参考算法程序化生成，不依赖 loose asset；新增官方 SearchTex 哈希、diagonal Area 抽样、45° 阶梯、shaderc、FrameGraph、view、Noop、teardown 与互斥回归。

> **2026-09-01 — 2D atlas sampling variants**: Forward2D accepts chunk meshes
> with baked atlas UVs. The per-draw payload also carries atlas texel size, and
> material creation selects Nearest, Linear, 4-tap, or 9-tap standard Phoskia
> source. This reuses the existing compiler/material path; it is not a new pass
> or shader file type. Sprites remain Linear by default.

> **文档状态**：2026-09-12 已对齐当前代码与可选高级效果执行策略；R0–R5 主管线已落地。
> **实现状态**：Forward 仍为产品默认，Deferred 通过 `makeDeferred()` 显式启用。Shadow、GBuffer、MotionVector、Lighting、Transparent、Bloom、DepthHaze、SSAO、PostProcess、TAA、FXAA、SMAA、ColorGrading、Present、UI 和 GBufferDebug 已接入 Pass 调度。PostProcess 产出 FrameGraph `FinalLdrColor`，TAA/FXAA/SMAA 与 ColorGrading 作为可选 LDR 节点提升 `PresentSource`，Present 保持唯一 backbuffer 边界；静态审核/修复已完成，等待真实 GPU capture 验收。
> **活动执行计划**：[`docs/execution-plan.md`](execution-plan.md)（P0–P6 队列、§5 segfault 约束、§5.4 隔离实验、附录 B/C/D 索引）。本文件是目标架构，与代码不一致时以代码与 execution-plan 为准。
> **关联文档**：[`docs/gbuffer-current.md`](docs/gbuffer-current.md)（当前 MRT/数据契约）、[`AYShader/design.md` §8.5](../AYShader/design.md)（opaque handle contract）、[`AYShader/README.md`](../AYShader/README.md)。

---

## 1. 概述

AYRenderer 负责：

- bgfx 生命周期（init / frame / shutdown）
- 视口、清屏、渲染状态
- 几何资源（VertexBuffer / IndexBuffer / Texture）创建与绑定
- 每帧收集可绘制对象，按 Pass 调度提交
- **通过 AYShader 的 `ShaderResource` 完成 shader 绑定与 program submit**（不自行调用 shaderc，不持有 `bgfx::ProgramHandle`）

### 1.1 设计原则

| 原则 | 说明 |
|---|---|
| **Shader 与 Renderer 解耦** | 材质侧只认 `ShaderResource` + `BindingId`；编译与 bgfx program 创建在 AYShader 内 |
| **bgfx 只在 Renderer 层** | Renderer TU 可 `#include <bgfx/bgfx.h>`；**禁止**在材质/游戏逻辑代码里 spread program/uniform handle |
| **Forward / Deferred 双路径** | Forward 保持默认与回归基线；Deferred 显式启用并作为主要扩展路径 |
| **单线程优先** | R1 直接调用；Command Queue 仅预留接口 |
| **小步可验证** | 每个 phase 有独立测试或 demo 场景 |

### 1.2 在引擎中的位置

```
┌────────────────────────────────────────────────────────────────────┐
│                         AYEngine / AYGameLoop                       │
├────────────────────────────────────────────────────────────────────┤
│  AYDevice                    AYRenderer                             │
│  (SDL 窗口 / 输入)            │                                    │
│       │ windowHandle          ├── BGFXAdapter (frame / vb / ib)    │
│       └──────────────────────►├── RenderPipeline (Pass 调度)       │
│                               ├── RenderScene (帧期场景快照)        │
│                               └── RenderResourceManager (GPU 资源)  │
│                                        │                            │
│                                        │ acquire / setUniform       │
│                                        ▼                            │
│                               ┌─────────────────┐                  │
│                               │  AYShader       │                  │
│                               │ ShaderResource  │                  │
│                               │ Pool            │                  │
│                               └────────┬────────┘                  │
│                                        │ pimpl → bgfx program       │
│                                        ▼                            │
│                               ┌─────────────────┐                  │
│                               │  bgfx           │                  │
│                               └─────────────────┘                  │
└────────────────────────────────────────────────────────────────────┘
```

---

## 2. 与 AYShader 的集成 Contract（必读）

本节是 Renderer 设计的 **spine**；与 [`AYShader/design.md` §8.5](../AYShader/design.md) 一致。

### 2.1 Frontend 应使用的类型

```cpp
namespace ayt::shader {

using BindingId = uint32_t;
constexpr BindingId InvalidBinding = 0;

struct TextureHandle {
    uint64_t id = 0;   // 见 §2.4
    bool isValid() const noexcept { return id != 0; }
};

struct DrawCallContext {
    uint8_t  viewId = 0;
    uint64_t state  = 0;   // bgfx 渲染状态位；0 = 不在 submit 内改 state
};

class ShaderResource;      // opaque；绑定 + submit
class ShaderResourcePool;  // 工厂 + cache + hot-reload + shutdown 回收

} // namespace ayt::shader
```

### 2.2 Frontend 不应使用的类型（材质 / 绑定路径）

| 禁止出现在 RenderMaterial / 游戏层 | 原因 |
|---|---|
| `bgfx::ProgramHandle` | 由 `ShaderResource` pimpl 持有 |
| `bgfx::UniformHandle` | 用 `BindingId` 间接操作 |
| `ShaderProgram`（legacy） | 已迁入 `detail/`；新代码禁用 |
| `BGFXConvertResult` / `.sc` 字符串 | 调试走 `CompileOptions::keepSources` |

Renderer **可以**在资源层使用 `bgfx::VertexBufferHandle` / `bgfx::TextureHandle` 等 **几何与图像** handle；与 shader 路径分离。

### 2.3 编译与获取 ShaderResource

```cpp
#include "AYShader.h"
#include "AYShader/Phoskia.h"

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

// 引擎启动：构造 pool，配置 shaderc / platform（一次）
ShaderResourcePool pool;
pool.setShadercExecutable(".../shaderc.exe");
pool.setPlatform("windows");
pool.setGLSLProfile("430");
pool.setAutoProbeFromRendererType(false);  // 或与 bgfx renderer type 联动
pool.setCacheDirectory(".../shader_cache"); // 可选

// 加载材质：从 Phoskia 源 acquire（带 source + binary 两级 cache）
ShaderResource mat = pool.acquire(phoskiaSource);

// 或经 Compiler 一站式（内部仍进 pool）
Compiler compiler;
ShaderResource mat2 = compiler.compileToShaderResource(phoskiaSource, CompileOptions{}, pool);

// 开发期 hot-reload
pool.setHotReloadEnabled(true);
pool.compileFromFile("shaders/unlit.phoskia");
// 每帧或定时：
pool.pollHotReload();
```

失败时 `ShaderResource::isValid() == false`；Renderer 应跳过 draw 并打日志，不 crash。

### 2.4 TextureHandle 约定（Phase 4  interim）

当前 AYShader 实现将 `TextureHandle.id` 的低 16 位映射为 `bgfx::TextureHandle.idx`：

```cpp
// AYRenderer 侧包装（R1 起放在 RenderTexture 或 BGFXAdapter）
inline shader::TextureHandle toShaderTexture(bgfx::TextureHandle h) {
    shader::TextureHandle out;
    out.id = h.idx;
    return out;
}
```

**后续（R2+）** 可改为 Renderer 维护 `TextureRegistry`，`TextureHandle.id` 为引擎侧 stable id，AYShader 内部再解析——design 预留，R1 沿用 idx 映射即可。

### 2.5 单次 Draw Call 顺序（bgfx 语义）

bgfx 要求 **同一 draw** 的 state / vb / ib / transform / uniform / texture 在 `submit` 之前设置完毕。

```
AYRenderer::drawMesh(...) 推荐顺序：

  1. BGFXAdapter::setViewRect / setViewClear          // 视口级（Pass 入口）
  2. BGFXAdapter::setTransform(worldMatrix)
  3. BGFXAdapter::setVertexBuffer(vb)
  4. BGFXAdapter::setIndexBuffer(ib)                  // 非 indexed 则跳过
  5. ShaderResource::setUniform(...)                  // 可多次
  6. ShaderResource::setUniformBlock(...)             // UBO（可选）
  7. ShaderResource::setTexture(stage, bindingId, tex)
  8. ShaderResource::submit(DrawCallContext{viewId, state})
         └── 内部：flush pending uniform/texture → bgfx::setState → bgfx::submit
```

**职责 split**：

- **BGFXAdapter**：步骤 1–4（几何与 transform）
- **ShaderResource**：步骤 5–8（shader 参数 + program submit）

`DrawCallContext::state` 非 0 时，`submit()` 内会 `bgfx::setState(ctx.state)`。Pass 级 state 与 per-draw state 合并策略在 R1 定为：**以 DrawCallContext 为准**；Adapter 不再单独 `setState` 除非 debug。

### 2.6 Binding 查询示例

Phoskia 声明名与查询名一致（property / uniform / texture / uniformblock 实例名）：

```cpp
ShaderResource& shader = material.shader();

BindingId tintId    = shader.getUniformBinding("tint");
BindingId albedoId  = shader.getTextureBinding("albedoMap");
BindingId cameraId  = shader.getUniformBlockBinding("Camera");

shader.setUniform(tintId, &color, sizeof(color));

shader::TextureHandle th = toShaderTexture(gpuAlbedo);
shader.setTexture(0, albedoId, th);

// UBO：用 getUniformBlockSize / getUniformBlockFieldOffset 填 staging buffer
shader.setUniformBlock(cameraId, cameraBlob, blockSize);

shader.submit({ .viewId = 0, .state = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS });
```

---

## 3. 核心架构

### 3.1 R1 最小模块（先实现这些）

```
AYRenderer
├── BGFXAdapter           # bgfx init/frame/view + vb/ib/transform
├── ForwardOpaquePass     # 唯一 Pass：清屏 + 不透明 forward
├── RenderScene           # 帧期 draw 列表（可极简）
├── RenderMesh            # vb/ib + layout
├── RenderMaterial        # ShaderResource + 绑定缓存
└── AYRenderer            # 入口：持有 Pool + Adapter + Pipeline
```

### 3.2 完整模块（R3+ 逐步引入）

```
RenderPipeline
├── PassManager
├── ForwardOpaquePass / TransparentPass   # R3+
├── ShadowPass / GBufferPass / ...        # R5+ 延后
DrawListBuilder                           # R3：按 shader/material 分组
RenderResourceManager                     # R2
CameraManager / LightManager              # R3
```

### 3.3 每帧流程（R1）

```
beginFrame()
  ForwardOpaquePass::execute()
    setViewRect / clear
    for each RenderItem in scene:
      adapter.setTransform(item.world)
      adapter.setVertexBuffer / setIndexBuffer
      bind material uniforms/textures
      material.shader().submit({viewId, state})
endFrame()  → bgfx::frame()
```

R3+ 再扩展为多 Pass 循环。

---

## 4. BGFXAdapter

### 4.1 职责边界

| 负责 | 不负责 |
|---|---|
| `bgfx::init` / `shutdown` / `frame` | shader 编译、program 创建 |
| view rect / clear / debug text | uniform 名解析 |
| `setVertexBuffer` / `setIndexBuffer` | `bgfx::createProgram` |
| `setTransform` | `bgfx::createUniform`（由 AYShader 在 acquire 时完成） |
| 创建/销毁 VB / IB / **GPU Texture** | |

### 4.2 接口（R1）

```cpp
namespace ayt::renderer {

struct BGFXInitParams {
    void*    nativeWindowHandle = nullptr;  // 来自 AYDevice
    uint32_t width  = 1280;
    uint32_t height = 720;
    bgfx::RendererType::Enum backend = bgfx::RendererType::Count;  // Auto
};

class BGFXAdapter {
public:
    bool initialize(const BGFXInitParams& params);
    void shutdown();

    void beginFrame();
    void endFrame();

    void setViewRect(uint8_t viewId, int x, int y, int w, int h);
    void setViewClear(uint8_t viewId, uint16_t flags, uint32_t rgba,
                      float depth = 1.0f, uint8_t stencil = 0);

    void setTransform(const ayt::math::Float4x4& world);
    void setVertexBuffer(bgfx::VertexBufferHandle vb, uint32_t start = 0,
                         uint32_t count = UINT32_MAX);
    void setIndexBuffer(bgfx::IndexBufferHandle ib, uint32_t start = 0,
                        uint32_t count = UINT32_MAX);

    // 资源创建（R1）
    bgfx::VertexBufferHandle createVertexBuffer(const void* data, uint32_t size,
                                                const bgfx::VertexLayout& layout);
    bgfx::IndexBufferHandle  createIndexBuffer(const void* data, uint32_t size);
    bgfx::TextureHandle      createTexture2D(/* ... */);

    void destroy(bgfx::VertexBufferHandle h);
    void destroy(bgfx::IndexBufferHandle h);
    void destroy(bgfx::TextureHandle h);

    // 数学类型 overload 的 setUniform 已删除 — uniform 走 ShaderResource
};

} // namespace ayt::renderer
```

> **与旧 design 的差异**：旧版 BGFXAdapter 含 `setUniform(handle, FVector3)` 与 `createProgram(vs, fs)`；现 **uniform/program 全部交给 AYShader**，Adapter 只处理几何与帧。

### 4.3 初始化与 AYDevice

```cpp
bool AYRenderer::initialize(const RendererSettings& settings) {
    auto* window = device->window();
    BGFXInitParams p;
    p.nativeWindowHandle = window->getNativeHandle();
    p.width  = window->getWidth();
    p.height = window->getHeight();
    p.backend = settings.backend;
    return _adapter.initialize(p);
}
```

窗口所有权在 **AYDevice**；Renderer 不创建 SDL 窗口。

---

## 5. RenderMaterial

### 5.1 职责

- 持有 **`ShaderResource`**（来自 pool.acquire）
- 缓存常用 **`BindingId`**（避免每帧字符串查找）
- 持有 **property 默认值**（与 Phoskia property 对应）
- **不**持有 `bgfx::ProgramHandle`

### 5.2 接口（R2 完整；R1 可极简）

```cpp
class RenderMaterial {
public:
    bool loadFromPhoskiaFile(ShaderResourcePool& pool, const std::string& path);
    bool loadFromPhoskiaSource(ShaderResourcePool& pool, const std::string& src,
                               const std::string& cacheKey = "");

    shader::ShaderResource& shader() { return _shader; }
    const shader::ShaderResource& shader() const { return _shader; }

    // 按 Phoskia 名字设置（内部查 BindingId）
    void setPropertyFloat(const std::string& name, float v);
    void setPropertyVec3(const std::string& name, const ayt::math::FVector3& v);
    void setTexture(const std::string& name, bgfx::TextureHandle tex);

    // 将 property + 外部 override 写入 shader pending buffer
    void flushBindings();

private:
    shader::ShaderResource _shader;
    std::unordered_map<std::string, shader::BindingId> _uniformBindings;
    std::unordered_map<std::string, shader::BindingId> _textureBindings;
    // property 默认值 ...
};
```

### 5.3 与 aymat 资源的关系（R2+）

```
aymat (数据)                    RenderMaterial (运行时)
┌─────────────────────┐        ┌──────────────────────────┐
│ phoskiaPath: "..."  │ ──────►│ pool.acquire(source)     │
│ baseColor, metallic │        │ property → setUniform    │
│ albedoTex: resId    │        │ texture  → setTexture    │
└─────────────────────┘        └──────────────────────────┘
```

**R1**：Phoskia 路径硬编码或 JSON 配置即可。  
**延后**：旧 design 中的 `material_shader_mapping` SQL 与属性→shader 启发式规则；待 aymat 格式稳定后再做。

---

## 6. Draw 路径数据结构

### 6.1 RenderItem（R1）

```cpp
struct RenderItem {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  indexBuffer  = BGFX_INVALID_HANDLE;
    uint32_t indexCount = 0;

    ayt::math::Float4x4 worldTransform;

    RenderMaterial* material = nullptr;   // 非 owning

    uint64_t stateOverride = 0;           // 0 = Pass 默认
};
```

### 6.2 DrawItem / DrawGroup（R3+）

旧 design 中 `DrawItem` 含 `bgfx::ProgramHandle program` — **已删除**。分组键改为：

```cpp
struct DrawGroupKey {
    uint64_t shaderResourceId;   // ShaderResource::id()
    uint32_t materialInstanceId;
    // ...
};
```

`DrawListBuilder` 在 R3 引入，R1 直接遍历 `RenderScene` 线性提交。

---

## 7. RenderScene

帧期快照；与 ECS **松耦合**（由 RenderSystem 填充，不强制 ECS 类型进 Renderer 头文件）。

```cpp
class RenderScene {
public:
    void clear();

    void addRenderable(/* entity id, transform, mesh ref, material ref */);
    std::span<const RenderItem> items() const;

    // R3+
    void setActiveCamera(/* ... */);
    void addLight(/* ... */);
};
```

R1 可只有一个 `std::vector<RenderItem>`，无 Camera/Light。

---

## 8. RenderPipeline & Pass

### 8.1 RenderPass 基类

```cpp
class RenderPass {
public:
    virtual ~RenderPass() = default;
    virtual std::string_view name() const = 0;
    virtual void execute(RenderContext& ctx, const RenderScene& scene) = 0;
    void setEnabled(bool e) { _enabled = e; }
    bool isEnabled() const { return _enabled; }
protected:
    bool _enabled = true;
};
```

### 8.2 R1：ForwardOpaquePass

```cpp
class ForwardOpaquePass : public RenderPass {
public:
    std::string_view name() const override { return "ForwardOpaque"; }
    void execute(RenderContext& ctx, const RenderScene& scene) override;
};
```

### 8.3 当前 Pass 管线（R5 已落地）

| Pass | 当前状态 |
|---|---|
| ShadowPass | 已接入 Forward/Deferred，支持关键光和每光源 atlas 数据 |
| GBufferPass | 已落地四颜色 MRT + D24S8，契约 v3 已冻结 |
| LightingPass | 已落地全屏 Deferred 光照；shadow atlas、完整多光 BRDF、采样变体与 HDR 链路已完成第二轮收敛 |
| TransparentPass | 已接入双路径；Deferred 借用 LightingOutput 颜色与 GBuffer 深度组成缓存 FBO，共享多光/阴影契约，按 sortKey 与相机距离稳定排序并在输入失效时 fail-close |
| Forward2DOpaquePass | Camera Overlay 2D；独立正交相机、无深度、跨 Sprite/Tilemap 全局稳定排序；Forward/Deferred 都在 3D Transparent 后合成 |
| BloomExtract / BloomBlur | 多尺度 HDR 金字塔：Karis 亮区提取，1/2→1/4→1/8→1/16 能量守恒降采样，tent 逐级上采样合并；完整链门控与当前帧产出契约保持 fail-close |
| AutoExposure | 独立 1×1 RGBA16F 历史 Pass，4×4 对数亮度估计与非对称时间适应；默认关闭，关闭时零资源、零 draw，RenderSettings 可切换并持久化 |
| DepthHaze | 已完成第二轮收敛：全分辨率 HDR HazeColor、Coverage 背景语义、逐帧 fail-close、透明 PBR 雾化与显式 view 顺序；SSAO 已在 Lighting 环境光阶段完成，不在 Haze 内重复合成 |
| SSAO | 已完成审核与按序修复：RT3 coverage、TBN 旋转核、view-Z 比较、完整链门控、生命周期闭合，且只影响 Lighting 环境光 |
| PostProcess / FXAA / ColorGrading / Present / UI | PostProcess 写 RGBA8 FinalLdrColor；FXAA 默认开启并可切换；ColorGrading 默认关闭，以 32³ 2D strip LUT 提供 Neutral/Warm/Cool/Cinematic；每个成功节点依次提升 PresentSource，Present 单独写 backbuffer，UI 最后合成 |
| GBufferDebug | Deferred-only，view 250，默认关闭 |

当前 Forward 默认顺序：

```text
Shadow → ForwardOpaque → DepthHaze(no-op) → Transparent
       → Forward2DOpaque → BloomExtract → BloomBlur
       → AutoExposure(optional) → PostProcess(FinalLdrColor) → FXAA(FxaaColor)
       → ColorGrading(ColorGradedColor) → Present → UI
```

当前 Deferred opt-in 顺序：

```text
Shadow → Skybox → GBuffer → MotionVector → SSAO → Lighting → DepthHaze
       → Transparent → Forward2DOpaque → BloomExtract → BloomBlur
       → AutoExposure(optional) → PostProcess(FinalLdrColor) → TAA / FXAA / SMAA
       → ColorGrading(ColorGradedColor) → Present → UI → GBufferDebug
```

### 8.4 UI 合批与绘制顺序

`UIRenderBackend` 先按 AYUI 产生的顺序记录整帧 `UiItem`，随后在 `flush()` 中选择提交顺序。默认的 `BatchMode::OverlapAware` 使用固定 96 项的前视窗口：后续兼容项只有在其与所有跨越项的绘制包围盒均不相交时才可前移。由此得到两个约束：

1. 任意重叠项的相对顺序不变，透明 UI、文字、阴影和边框仍遵守 painter order。
2. 不相交区域允许按纹理、state 或 SDF 参数聚拢，减少 submit 与状态切换。

兼容键如下：

| 类型 | 合批键 |
|---|---|
| Flat | bgfx state + texture |
| SDF | bgfx state + 完整 SDF 参数 |

调度器只生成逻辑索引，不改写原始 item 数组；实际 vertex/index 数据仍按既有路径构建和提交。窗口大小为常数，因此相对 item 数量的渐进复杂度保持线性。非有限包围盒、过大输入或内部校验失败会自动使用原始输入顺序。

Debug 构建还会对调度结果执行独立的不变量校验：结果必须是输入索引的完整排列，且每一对
相交 item 的 painter order 必须保持不变。该检查为 O(N²)，仅用于测试与开发构建；失败时
清空调度结果并回退到保守顺序，不进入 Release 热路径。

`BatchMode::OrderedRuns` 完整保留旧实现：只合并相邻且兼容的 item。该模式作为运行时兜底、排障开关和性能 A/B 基线存在，不需要维护第二套渲染后端。

### 8.5 Production UI Layer 与共享 RenderTargetPool

Renderer 在 adapter 初始化后创建 renderer-wide `RenderTargetPool`，并把同一实例注入 FrameGraph 与
`UIRenderBackend`。这不是“UI 专用缓存池”和“Pass 专用缓存池”两套实现；物理 FBO 的创建、复用、
延迟回收、预算和 reset 生命周期只有一个 owner。shutdown 顺序固定为：消费者释放 lease → pool
shutdown → adapter shutdown；resize/MSAA 切换顺序固定为：FrameGraph 释放 → pool reset → bgfx reset。

池的 key 为 `(width, height, colorFormat, withDepth, sampleCount, pointSampled)`，只做精确匹配。lease 由 slot 与
generation 组成，release 后旧 handle 立即失效；物理目标默认 quarantine 两帧，避免 CPU 已结束但
bgfx 队列仍引用 attachment。256 MiB 预算只淘汰已空闲且越过 quarantine 的 LRU 目标。`acquire(key,
true)` 是 FrameGraph 的 soft/best-effort 路径，允许既有 leased/quarantined 资源令预算暂时超限；
`acquire(key, false)` 是 UI 的 strict 路径，先把 idle LRU 清到 `budget-request`，仍放不下就拒绝分配。
当前明确拒绝 sampleCount != 1。

`UIRenderBackend` 实现完整 RenderTarget create/resize/release/bind/texture/blit 和 Layer
create/update/release/paint/composite/invalidate。Layer 使用 RGBA8 + depth/stencil backing target；
createLayer 只创建稳定的逻辑 handle，backing 在首次 paint 时按 strict budget 懒申请。申请失败时，
backend 选择最久未 composite、且当前未 paint 的 Layer 撤销 backing；受害 Layer 保持 handle 并标脏，
本帧调用方 immediate fallback，后续帧跨过 quarantine 后可复用 target。由此压力降级不制造第二套
Widget 绘制路线，也不会把 dangling target token 暴露给 AYUI。
逻辑 bounds 的 min/max 分别按 DPI 向外 `floor/ceil` 到物理像素，二者之差决定物理尺寸；完整
backing composite 后裁回 logical bounds。这个约束使小数 origin/extent 的离屏像素中心仍与主
framebuffer 对齐，不能退回只对 logical width/height 做 `ceil`。offscreen paint 当前使用 view 26–243
（244/245 留给 Selection/TAA diagnostics，250 留给 GBufferDebug），主
composite 使用 view 255。
target 切换是 batch barrier：进入离屏前 flush，保存 canvas/clip/path-clip/opacity/blend 状态；结束
paint 后 flush 并恢复。由此 flat、SDF、text、nine-patch、vector fill/stroke 和 nested stencil clip
继续走同一条生产图元路径。

UI RenderTarget 申请 point-sampled backing，保证同物理尺寸 composite 保持 texel identity；FrameGraph
目标默认仍为 linear，并由 pool key 隔离。RenderTarget composite 的 V 方向读取
`bgfx::Caps::originBottomLeft`，OpenGL 不再把 Layer 倒置；point-sampled glyph bitmap 的最终 quad
吸附物理像素网格，pen/kerning advance 仍保持小数精度，保证默认 framebuffer 与 FBO 在 1.0×/1.5×
下使用一致 coverage。Layer paint 对 straight-alpha 输出使用 RGB
`SRC_ALPHA/INV_SRC_ALPHA`、alpha `ONE/INV_SRC_ALPHA`，使目标保存 premultiplied RGB 和正确 coverage；
composite 再使用 premultiplied-over，不能对 alpha 做第二次相乘。局部 damage replay 必须保持原图元
参数空间：纹理按 clip fraction 重映射 UV，四角渐变按原 bounds 双线性重映射颜色。
Color clear 的全量 view clear 与局部覆盖 clear 都先写入 `(rgb*alpha, alpha)`；Additive、Multiply、
Screen 保留各自 RGB 方程，但 alpha 独立使用 `ONE/INV_SRC_ALPHA` coverage source-over。直接使用 bgfx
Multiply/Screen convenience state 会把 RGB factor 复用于 alpha，破坏不透明隔离层，属于错误实现。

文字路径按 Unicode grapheme 做跨 face fallback；一个 cluster 不会被拆到多个字体。常用 emoji、symbol、
CJK family 预热，其他系统字体按首次缺字按需注册并缓存。每个 shaped run 携带自己的 font/atlas key，
测量、cluster source offset、换行和绘制共用同一序列；RTL 会同步反转 font-run 视觉顺序。普通 glyph
以白色 coverage 写 BGRA8 atlas 并由顶点色着色，FreeType color bitmap 先从 premultiplied BGRA 转为
straight alpha，再以白色顶点色提交，匹配 UI pass 的 straight-alpha source-over。

当前字体渲染的产品目标与发布门禁以 Windows 为准。Linux/macOS 的系统字体枚举、fallback 字体集差异、
彩色 Emoji/可变字体和真实后端组合验证进入最低优先队列；跨平台代码路径继续保持可构建、可扩展，
但现阶段不承诺与 Windows 相同的实机覆盖，也不以补齐该矩阵阻塞 Windows 主线。

AYUI 的 root Production Layer 是 opt-in。首次、无范围 dirty、resize/DPI/reset 帧完整绘制主树；
显式 dirty rect 由 sidecar 保留最多 8 个独立 damage region 并逐区 replay；重叠/相邻区域合并，
第 9 个区域退化为 union，累计面积达到 layer 的 70% 时 full repaint。Widget 的 Always/Auto policy
可对复杂子树使用同一 Layer 能力，Auto 由稳定帧、面积、display-command 数和连续 invalidation
决定晋升/降级。Transparent/Color Layer 先用无混合覆盖写清除 damage，
保留区域外像素；Preserve Layer 跳过清除。clean 帧只 composite 一次；overlay 与 drag visual 随后
即时绘制。capability、target 或 paint 失败时 AYUI 同帧回退原始路径。当前每帧最多 224 次
offscreen paint，第 225 次确定失败，下一帧重新从 view 26 分配。该上限只描述 pass 数量，不承诺
UI 独占 224 个共享 framebuffer。`LayerCacheStats` 汇总 layer paint/composite/cache hit/physical
repaint area、allocation failure/degradation，并拼接 pool allocation/reuse/eviction、live/idle、
allocated/budget bytes；reset stats 不销毁资源。池与 UI backend 均限定 renderer thread。Noop 测试
锁定生命周期和 submit 形态；D3D11/D3D12/Vulkan/OpenGL 各运行 36 次独立 capture：原 1.0×/1.5×
复杂控件路径，以及透明 Layer、
group/nested opacity、Additive/Multiply/Screen 隔离组、resize、动态 DPI、device/MSAA reset lease
恢复和 Transparent/Color/Preserve 局部 clear。clean reuse、isolated blend、Preserve 字节精确；
通常最多差 1 LSB，RGBA8 group opacity 因离屏与最终合成两次量化最多差 2 LSB。MSAA 后 Layer 仍是
1× sample，因此生命周期门禁比较 recovered Layer 与 reset 后 fresh Layer，不拿 multisampled immediate
边缘作为错误参考。`RunLayerVisualRegressionMatrix.ps1` 默认串行运行 D3D12/Vulkan/OpenGL 并汇总
失败，单后端脚本仍可用于 D3D11 或诊断。

### 8.6 可选高级效果的配置与执行边界（2026-09-12）

高级效果采用“统一配置入口、按数据依赖分组执行”的设计，不建立包含全部算法的单一
`AdvancedEffectPass`。配置层可以向 Editor 和游戏暴露同一组开关与参数，但配置分组不等于 GPU Pass：
具体执行单元由输入资源、执行时序、分辨率、历史状态和中间结果决定。候选效果本身由
[`AYRendering-Architecture-Roadmap.md` §8.1](../../AYDocs/AYRendering-Architecture-Roadmap.md)
维护，本节只规定 AYRenderer 的工程边界。

建议的配置分组为 `ColorStylization`、`Outline` 与 `TemporalFeedback`。该形态是未来 API 方向，
不要求直接扩展 `FrameContext` POD；结合既有 MSVC stale object/ABI 事故，新增参数优先保存在
Renderer-owned sidecar 或对应 Pass 对象中，经清洗后逐帧上传。

#### 合并与拆分规则

- 输入颜色、输出格式、分辨率和执行位置相同，且不拥有历史/中间资源的逐像素颜色运算可以融合。
  色板量化、黑白/双色映射、posterize、简单 dithering、scanline、grain 与轻量色差属于候选
  `StylizedColor` 组。融合后固定内部处理顺序，避免设置顺序隐式改变画面。
- 读取 GBuffer Depth/Normal/Material 数据的轮廓与蓝图效果独立成 `StylizedOutline` 组；隐藏线若需要
  额外几何或深度层，不能伪装成普通颜色滤镜。边缘 mask 的生成与最终 composite 可以是同一
  C++ Pass 所有的多个 GPU stage。
- 使用持久 history、Motion Vector 或多帧 ring 的冻结、残影、时间回声、Datamosh 与 slit-scan
  独立成 `TemporalFeedback` 组。它们的失效、resize、camera cut 和 scene/pipeline switch 规则
  不得泄漏到无历史颜色效果。
- Kuwahara、水彩扩散、大半径卷积和其它需要多轮邻域采样或降采样链的算法保持独立能力；一个
  C++ Pass 可以拥有多个 stage，不能为了表面上的“单 Pass”强行压成一次 draw。
- Bayer、Blue Noise 与 Spatiotemporal Blue Noise 归入共享 `NoiseSequenceProvider` 能力，而不是
  固定画面 Pass。SSAO、随机透明、dithering 和 temporal hold 可以消费同一确定性 seed/frame
  序列。TAA 继续使用低差异投影 jitter；是否改变其 Halton 序列必须单独验证，不能由风格化
  dithering 设置间接改变。

建议的逻辑顺序为：

```text
PostProcess(FinalLdr) -> TAA/FXAA/SMAA
    -> optional Outline composite -> ColorGrading
    -> optional StylizedColor -> optional TemporalFeedback
    -> Present -> Editor/UI
```

Outline 的 edge mask 可以读取更早产生的 GBuffer，但最终在 AA 前还是 AA 后 composite 由视觉原型
验证决定；选择框、Gizmo、方向轴和 Editor/UI 始终位于高级效果之后，不进入游戏画面 history。
TemporalFeedback 只在最终合成时选择/重投影 current 与 history，绝不能通过跳过 simulation、场景
提交、renderer frame advancement 或 backend present 来制造“掉帧”效果。

#### 开关、Shader variant 与零开销旁路

关闭行为分为三层，必须优先使用最外层裁剪：

1. **FrameGraph gate**：某一效果组全部关闭时，不声明逻辑输出、不申请 transient RT、不添加 live
   节点，`PresentSource` 直接沿用上一生产者；禁止用 identity blit 表示关闭。
2. **Pass gate**：已挂载但 disabled 的 Pass 由 `RenderPipeline::executeAll()` 跳过，不进入
   `execute()`，不产生 draw、dispatch 或 GPU state mutation。
3. **Shader gate**：只有同组内仍有其它操作开启时，才允许使用 uniform strength/flag 跳过轻量
   算法。需要额外 sampler、history、GBuffer 或显著控制流的差异使用少量结构性 variant；不为每个布尔
   组合生成 `2^N` 个排列。

默认从未启用的高级效果不得编译 shader、创建 history/LUT/noise 私有资源或占用 RenderTargetPool
lease。某效果开启过再关闭时，Pass 可以缓存 immutable shader/LUT 以降低二次开启卡顿；history
纹理必须标无效，是否立即释放由显存预算策略决定，但关闭期间 GPU frame cost 仍为零。首次开启
可能触发 shader 编译和目标创建；产品若要求无卡顿切换，应在 loading 阶段预热指定 variant，而不是
让所有可选效果随 Renderer 初始化无条件编译。

在 RGBA8 下，一次最小全屏 source read + target write 的理论流量约为 1080p 每帧 16.6 MB、4K
每帧 66.4 MB；60 FPS 时分别约 1 GB/s 与 4 GB/s，尚未包含多 tap、缓存失效和 HDR 格式。因此
同位置的轻量颜色运算应尽量融合，跨资源/跨时序效果则以正确生命周期优先，不能只为减少 draw
数量破坏契约。

#### 验收不变量

- 全部高级效果关闭时，FrameGraph live set、GPU submit 数与 presentation 字节结果保持基线一致。
- 单组关闭不保留孤立输出或无消费者历史依赖；节点失败时不提升 `PresentSource`。
- 时间类效果使用可重复 seed，并在 resize、camera cut、scene/pipeline switch、backend reset 与
  disabled-to-enabled 时明确初始化 history，不读取旧尺寸或旧场景内容。
- 高级效果默认不影响 Editor/UI；需要影响 UI 的产品特效必须另设显式合成策略。
- 性能验收同时记录 GPU 时间、全屏读写次数、瞬态/持久显存和首次启用耗时，不能只比较 draw count。

---

## 9. AYRenderer 主类

```cpp
namespace ayt::renderer {

struct RendererSettings {
    bgfx::RendererType::Enum backend = bgfx::RendererType::Count;
    bool enableDebugText = false;
    // R5+：enableShadows, enableDeferred, ...
};

class AYRenderer {
public:
    explicit AYRenderer(ayt::device::DeviceManager* device);
    ~AYRenderer();

    bool initialize(const RendererSettings& settings);
    void shutdown();

    void beginFrame();
    void renderFrame(const RenderScene& scene);
    void endFrame();

    BGFXAdapter& adapter() { return _adapter; }
    shader::ShaderResourcePool& shaderPool() { return _shaderPool; }
    RenderPipeline& pipeline() { return *_pipeline; }

    // R4
    void pollShaderHotReload() { _shaderPool.pollHotReload(); }

private:
    ayt::device::DeviceManager* _device = nullptr;
    BGFXAdapter _adapter;
    shader::ShaderResourcePool _shaderPool;
    std::unique_ptr<RenderPipeline> _pipeline;
};

} // namespace ayt::renderer
```

> **与旧 design 的差异**：不再内嵌 `AYShader m_shaderSystem` 或 `ShaderProgram*`；**`ShaderResourcePool` 为唯一 shader 运行时入口**。需要 AST 级编译时用 `phoskia::Compiler`，但 product 路径仍 `pool.acquire`。

---

## 10. 与其他模块

### 10.1 AYShader

| Renderer 调用 | 时机 |
|---|---|
| `pool.setShadercExecutable` / `setPlatform` / `setGLSLProfile` | 初始化 |
| `pool.acquire(path \| src)` | 材质加载 |
| `shader.get*Binding` | 材质 load 后缓存 |
| `shader.setUniform` / `setTexture` / `submit` | 每 draw |
| `pool.pollHotReload` | 每帧或 debounce |
| `pool.shutdown` | `AYRenderer::shutdown` 中、**bgfx::shutdown 之前** |

生命期顺序：

```
AYRenderer::initialize  → bgfx::init
                        → pool 配置
AYRenderer::shutdown    → pool.shutdown()   // 释放 bgfx shader handles
                        → adapter.shutdown() → bgfx::shutdown
```

### 10.2 AYDevice

- 提供 `nativeWindowHandle`、`width`、`height`、resize 事件
- Renderer 订阅 resize → 更新 `bgfx::reset`

### 10.3 AYResource（R2+）

`RenderResourceManager` 从 AYResource 加载 aymesh / aytex，上传 GPU，返回 `RenderMesh` / `bgfx::TextureHandle`。

### 10.4 AYEntity / ECS（Engine 集成，2026-07 落地）

```
EntitySubSystem::update()
  → World::update() → RenderSystem::onStart / onUpdate
RenderSystem::buildRenderScene()
  → query<Transform, MeshComponent>
  → Renderer::loadMesh / loadMaterial
  → RenderScene::add(mesh, material, worldMatrix)

GameLoop::submitRenderCommands()
  → RendererSubSystem::renderFrame()
  → scene builder callback → Renderer::render(scene)
```

| 类 | 职责 |
|---|---|
| `RendererSubSystem` | GameLoop 子系统；持有 `Renderer`；注册 render callback |
| `RenderSystem` | ECS System；填充 `RenderScene` |
| `bootstrapModule()` | 静态库显式注册 Entity 子系统 + RenderSystem + 组件类型 |

**Bootstrap API**（`AYRenderer/RendererSubSystem.h`）：

```cpp
RendererSubSystem::setBootstrapWindow(hwnd, w, h);
RendererSubSystem::setBootstrapShaderDumpDirectory(dir);  // 可选，initialize 后生效
```

**Shutdown 顺序**（避免静态析构崩溃）：

1. `GameLoop::shutdown()` → 逆序 `ISubSystem::shutdown()`
2. `RendererSubSystem::shutdown()`：清空 render callback → `Renderer::shutdown()`
3. `ShaderResourcePool` registry 使用 intentionally-leaked 堆存储，避免进程退出时 static 析构顺序问题

Renderer **不**依赖 ECS 头文件；只消费 `RenderScene`。

---

## 11. 多线程扩展（预留）

R1 不实现。接口预留：

```cpp
class IRenderCommandQueue {
public:
    virtual ~IRenderCommandQueue() = default;
    virtual void flush(BGFXAdapter& adapter, shader::ShaderResourcePool& pool) = 0;
};
```

---

## 12. 目录结构

### 12.1 R0–R1 最小树

```
AYRenderer/
├── README.md
├── design.md
├── CMakeLists.txt
├── include/AYRenderer/
│   ├── AYRenderer.h
│   ├── BGFXAdapter.h
│   ├── RenderPipeline.h
│   ├── RenderPass.h
│   ├── ForwardOpaquePass.h
│   ├── RenderContext.h
│   ├── AYRenderer/RenderScene.h
│   ├── RenderAYResource/AYResource/assetsImpl/Mesh.h
│   ├── RenderAYResource/AYResource/assetsImpl/Material.h
│   └── RendererSettings.h
├── src/
│   ├── AYRenderer.cpp
│   ├── BGFXAdapter.cpp
│   ├── RenderPipeline.cpp
│   ├── ForwardOpaquePass.cpp
│   ├── RenderScene.cpp
│   ├── RenderMesh.cpp
│   └── RenderMaterial.cpp
└── unittest/                    # R1 末
    ├── CMakeLists.txt
    └── Test_ForwardOpaque.cpp   # headless 或 demo window
```

### 12.2 R3+ 扩展

```
include/AYRenderer/
├── Draw/DrawListBuilder.h
├── src/detail/RenderResourceManager.h
├── Entity/CameraManager.h
└── Passes/ShadowPass.h ...
```

---

## 13. 实现路线图

### Phase R0 — 设计对齐

- [x] 修订 `design.md` 对齐 AYShader Phase 4
- [x] 添加 `README.md` 状态表
- [x] `CMakeLists.txt`

### Phase R1 — 最小上屏

- [x] `BGFXAdapter`：init / frame / view / vb / ib / transform
- [x] `ShaderResourcePool` wiring（shaderc 路径、platform 430）
- [x] `RenderMaterial`：`pool.acquire` + texture / property
- [x] `ForwardOpaquePass`：mesh + Phoskia material
- [x] 验证 draw 顺序 §2.5
- [x] 测试 + Demo

**验收**：屏幕出现 lit mesh；公开头文件无 `bgfx::ProgramHandle`。

### Phase R2 — 资源管理

- [x] `RenderResourceManager`：mesh/texture 缓存
- [x] AYResource bridge：aymat / aymesh / aytex
- [x] `SimpleLit` 级 material 跑通

### Phase R3 — 相机与光照

- [x] `setMainCameraLookAtPerspective`：view/proj → uniform
- [x] 方向光 frame uniform
- [x] `TransparentPass`

### Phase R4 — 工具链

- [x] `pollHotReload` / `pollShaderHotReload`
- [x] debug overlay（FPS / draw stats）
- [x] `captureScreenshot`

### Phase Engine — GameLoop + ECS（2026-07）

- [x] `RendererSubSystem` + `REGISTER_SUBSYSTEM`
- [x] `RenderSystem` + `bootstrapModule()` 引导
- [x] `AYEngineIntegration_Demo`：旋转 ECS 立方体 + overlay
- [x] 单线程 render callback（`setRenderThreadEnabled(false)`）
- [x] `UIRenderBackend` overlap-aware 合批 + `OrderedRuns` 兜底
- [x] Production UI Layer + renderer-wide RenderTargetPool（FrameGraph/UI 共享）

### Phase R5+ — Deferred 与高级 Pass

- [x] Shadow / GBuffer / Lighting / Transparent / PostProcess / FXAA / ColorGrading / Present
- [x] Bloom / DepthHaze / SSAO / Skybox / GBufferDebug
- [x] GBuffer v3：帧有效性、资源所有权、逆转置法线、StandardLit/Unlit、AO/Model/Coverage 分离
- [ ] 真实 D3D11/12 GPU capture：MRT 值、Unlit/Cutout/Skinning/非均匀缩放与带宽
- [x] LightingPass 第一轮静态审核：架构、数据流、光照方程、阴影、生命周期与失败契约
- [x] LightingPass 第二轮收敛：每槽独立 Shadow view/VP、有效灯数与有限值、8 灯完整 BRDF、阴影采样变体、tile 安全采样、RGBA16F 中间链路
- [ ] LightingPass 真实 D3D11/12 GPU 验证；Point 全向阴影与 Spot 透视锥体阴影另立能力项
- [x] TransparentPass 第一轮静态审核：双路径目标、排序、深度/混合状态、光照与阴影契约、资源生命周期
- [x] TransparentPass 第二轮收敛：Deferred 借用附件、输入 fail-close、相机距离稳定排序、正确 alpha 语义、共享 8 灯/atlas/IBL 上传与 FBO 缓存失效
- [ ] TransparentPass 真实 D3D11/12 GPU capture：叠层玻璃、straight/premultiplied/additive、多光 atlas、resize/MSAA 缓存失效
- [x] BloomExtract/BloomBlur 第一轮静态审核：链路门控、数据流、HDR/曝光、失败路径、shader 质量与测试有效性
- [x] BloomExtract/BloomBlur 第二轮收敛：完整链零分配门控、逐帧产出 latch、参数清洗、HDR Karis 降采样、5-fetch/axis blur、Final fail-close
- [ ] Bloom 真实 D3D11/12 GPU capture：阈值/knee、曝光一致性、resize、格式 fallback、边缘采样与高亮稳定性
- [x] DepthHaze 第一轮静态审核：资源所有权、深度/背景语义、SSAO/透明/Bloom 顺序、失败路径与测试真实性
- [x] DepthHaze 第二轮收敛：全分辨率 HazeColor、Coverage 背景策略、AO-before-fog、逐帧 latch、透明 PBR 雾化与参数清洗
- [ ] DepthHaze 真实 D3D11/12 GPU capture：天空/地平线、遮挡边缘、透明叠层、additive、resize 与 HDR 格式 fallback
- [ ] 场景渲染 `DrawListBuilder` 合批（UI 合批已独立完成）
- [ ] Command Queue
- [ ] `material_shader_mapping` 数据库
- [ ] 从 AliyatRenderer 迁移 2D/UI/Skybox（**单独评估**；旧栈为 OpenGL，非直接移植）

---

## 14. 测试与生命周期约定（2026-07-20 补）

本节锁定 **测试 / CI 上不可忽略的两条 invariant**。两处都是真实坑,后人不要当偶发神 bug 重新踩。

### 14.1 Sticky Noop 后端 + 进程级 refcount

- `BGFXAdapter` 进程级 ref-count:同进程里首次 `initialize(Backend::Noop)` 锁 Noop,后续任何 `initialize(Backend::* | Auto)` 都返回 invalid(由 `57908fd` 一类修复引入,**未证明修干净**)。
- 一旦 Noop 锁上,**该进程任何后续 Renderer 都走 Noop**,即使显式要 Direct3D11 也无效。
- 这是 sticky 而非可恢复:若想真 GPU 必须 **新开进程**(独立 test exe / 新 fixture child)。
- **CI 落地:** `AYRenderer_Test` 永远 Noop(锁 sticky);真 GPU 跑窗口 Demo(`AYRenderer_Demo` / `AYEngineIntegration_Demo`);不要试图在 Noop 锁定的进程里切到真 backend。

### 14.2 Shaderc + 多 Renderer 实例

- `Test_RenderResources::textured_material_draw_one_frame` 创建 **第二个** Renderer → `createMaterialFromPhoskia` 触发 shaderc 子进程。
- 子进程路径上的 SIGSEGV / exit 139 bisect 多次稳定出现,与 Shadow 改动无强相关(见 `docs/execution-plan.md` §5)。
- 多 Renderer / 多 shaderc 子进程的组合在 Noop + Windows 子进程 pipe 上仍有 flaky 残余。修 ABI / 加 Pass / 切 FrameContext 签名时若撞上,**先跑 §5.4 隔离实验 ≥3 次**确认是否新引入,不要默认归 pre-existing rot 草率合并。

### 14.3 测试锚点

每次 PR 必跑:

- `Test_LightingCamera`(`forward_opaque_draw_one_frame` 类的现网回归锚)
- `Test_RenderResources::textured_material_draw_one_frame`(双 Renderer + shaderc)
- `Test_PostProcess_R5Plus_*`(R5.1 wire + P2 改动)
- `Test_ShadowPass`(cut-1 + 未来 cut-2)
- `Test_UIPass_AI1`(composite + UI view 255 锁)
- `Test_RenderTargetPool`(generation、quarantine、精确键复用、预算淘汰、reset)
- `Test_UILayerRenderTarget`(复杂图元/path clip、多区域 damage、clean 单 composite、strict budget/LRU
  降级、224 次离屏 pass 溢出/跨帧恢复、resize/MSAA reset 重绘)

跑法:同一 commit **连续 3 次**全量 `AYRenderer_Test`,记录 PASS/FAIL;**3 次不全绿**则按 `docs/execution-plan.md` §5 处理,不许合并带赌的 ABI 变更。

2026-08-29 Production UI Layer/RenderTargetPool 阶段的 Windows Debug Noop 全量基线为
`3334 / 3334`。该结果验证 API、生命周期、view/submit、strict budget 和压力降级契约；四后端
36-capture 图像矩阵补充像素正确性，但仍不替代 RenderDoc capture。

### 14.4 MSVC 增量对象与内部 ABI

- 修改 Pass 类布局、显式/隐式构造函数或 `PassExecContext` 尾字段后，如果出现随机 bool、内存断言、重复构造符号或测试进程挂住，先怀疑旧 `.obj` 与新头文件布局混用。
- 本轮 DepthHaze 曾由旧测试对象携带旧的隐式构造函数，导致 `_producedThisFrame` 初值随机；加入显式构造后，链接器进一步报告同一构造函数重复定义，最终通过清理并完整重建 `AYRenderer_Test` 目标消除。
- 此类问题不得靠放宽断言或增加运行时容错掩盖。先终止精确的残留测试进程，再对目标做干净重建；调试测试保持串行，避免多个断言窗口与 exe 文件锁互相干扰。

---

## 14. 与旧版 design 的主要变更摘要

| 旧设计 | 新设计 |
|---|---|
| `AYShader` + `ShaderProgram*` | `ShaderResourcePool` + `ShaderResource` |
| `DrawItem.program = bgfx::ProgramHandle` | 仅 `RenderMaterial::shader()` |
| `BGFXAdapter::createProgram` | 删除；`pool.acquire` |
| `BGFXAdapter::setUniform(bgfx::UniformHandle, ...)` | 删除；`ShaderResource::setUniform(BindingId, ...)` |
| Phase 1 含 GBuffer + Shadow | R1 仅 ForwardOpaque |
| `material_shader_mapping` SQL | R2+ 显式 phoskia 路径；SQL 延后 |
| Renderer 内嵌 `AYShader` 对象 | 持有 `ShaderResourcePool`；`Compiler` 按需局部使用 |

---

## 15. 参考

- [AYShader README](../AYShader/README.md)
- [AYShader design §8.5](../AYShader/design.md)
- [AYDevice design](../AYDevice/design.md)
- [bgfx API](https://bkaradzic.github.io/bgfx/)
- bgfx examples：`01-cubes`（R1 几何参考）

---

## 16. 变更记录

### 2026-09-12 — 可选高级效果执行策略

- 统一 Editor/游戏配置入口，但按颜色、GBuffer 轮廓、时间历史和多阶段邻域处理的数据依赖拆分执行；
  不采用包含全部算法的 mega-pass。
- 明确 FrameGraph gate → Pass gate → Shader gate 的三级旁路；全部效果关闭时不新增 RT、GPU submit
  或 identity blit，首次资源创建保持 lazy，并允许产品 loading 阶段有选择地预热。
- Blue/Spatiotemporal Blue Noise 定位为共享采样能力，TAA Halton 投影 jitter 不随风格化设置隐式改变；
  时间反馈不得通过跳过引擎帧更新实现，Editor/UI 保持在高级效果之后。

### 2026-09-01 — FXAA Quality 修复

- 原 `fxaa_311_luma_v1` 是无低对比度 early-out 的中心/四角方向滤波，视觉上会把高频细节变软，却仍难以处理长斜边阶梯。实现先替换为 `fxaa_quality_edge_search_v2`，随后由本日色度边缘修复升级为 `fxaa_chroma_edge_search_v3`；FrameGraph、view 17、ABI、运行时开关和 `PresentSource` 事务提升契约均保持不变。
- 新 shader 先以中心和 N/E/S/W 五点共同计算亮度与 RGB 最大通道差，使用二者最大范围进行边缘门控；低对比度像素精确返回中心值。确认边缘后读取四角，以亮度/色度二阶导数共同完成水平/垂直分类，再沿切线以 1/2/4/8 像素搜索端点，最终只沿法线做一次双线性偏移采样。
- sub-pixel correction 固定为较克制的 0.50，端点完成阈值为主梯度的 0.25；质量参数通过 `fxaaQuality` 上传，保留后续接入 Render Settings 调参而无需生成 shader 变体的空间。
- FXAA 保留在既有 raw bgfx `.sc` 编译路径，作为受控兼容例外。Phoskia 已能生成 `if/else`，但仍缺少用户函数，且 material `return` 是输出槽赋值而非真正早退；直接迁移会改变非边缘 early-out 与端点搜索语义。该 raw 路径由 shaderc `windows/s_5_0` 同时覆盖 D3D11/D3D12。
- `AYRenderer_FXAA` 更新为 55/55，覆盖对比度门控、边缘方向、1/2/4/8 搜索上界、参数默认值、UV clamp 与 VS/FS 生产编译；完整 `AYRenderer_Test` 为 3661/3661。视觉 A/B 继续使用 Editor 现有 FXAA 勾选框，本刀未触碰存在并行修改的 AYEditor/AYUI。

### 2026-08-31 — Editor Transform Gizmo Overlay

- `EditorOverlayPass` 在原方向轴 view 251 之外启用 view 252，绘制选中实体的程序化 Universal Transform Gizmo；三轴平移箭头、三个平面方片、三轴旋转圆环、三轴缩放方块与中心统一缩放方块同时显示，不依赖贴图或模型资源。
- AYEditor 通过 `Renderer::setEditorTransformGizmoState` 只传输可见性、模式、Local/World、活动 handle、投影退化禁用位掩码、位置和旋转；Renderer 在 pipeline rebuild 后恢复该状态，编辑器交互与 GPU 几何所有权保持分离。
- Gizmo 尺寸按相机距离缩放以保持近似稳定的屏幕占比；Universal World 模式把平移/旋转与局部缩放拆为两个 index range/draw，前者使用单位旋转、后者使用实体旋转，Local 模式可合并为一次 draw。handle hover/drag 使用稳定数值 ID，并将命中部分高亮为黄色。
- Universal 的几何按径向分层：缩放使用约 `0.09–0.38` 的短细杆和内侧小方块，平移只绘制为约 `0.50–0.95` 的长外段细箭头，旋转环位于半径 `1.03` 的最外圈；平面手柄由实心方片改为细边框。X/Y/Z 色相仍统一表示坐标轴，操作类型由方块/箭头/圆环形状及所在层级区分。
- view 252 在 Present 后、UI 前只清 depth、不清 color，Gizmo 始终可操作且内部保持深度遮挡；动态 VB/IB 只在 Gizmo 模式、高亮 handle 或禁用位掩码变化时重建，位置/旋转变化仅更新 model transform。禁用 handle 使用保留原轴色相的 36% RGB 暗色，不依赖 alpha blending。
- CPU 拾取、投影退化判定和 Transform 约束属于 AYEditor 的 `EditorTransformGizmo`，Renderer 只消费同一禁用位掩码并给出视觉反馈，不反向参与输入。`AYRenderer_EditorOverlay` 覆盖公开状态、view 252 和 pipeline rebuild 持久化；Renderer 全量回归为 3648/3648。

### 2026-08-30 — FXAA LDR Pass

- 新增 append-only `RenderPassSlot::FXAA=16`、`FgResourceId::FxaaColor=7` 与 `FXAAPass`。默认、Deferred 和两条 Editor 管线固定为 PostProcess → FXAA → Present；旧 custom descriptor 不会被强制加入 FXAA，但显式包含 FXAA 且缺少 Present 时会把 Present 补在 FXAA 后。
- FXAA 使用稳定 view 17，`RenderViewOrder` 显式将它排在 PostProcess view 15 与 Present view 16 之间，并与 Shadow atlas 18–25、当前 UI Layer 26–243、Selection/TAA diagnostics 244/245、GBufferDebug 250、Editor 251–254 隔离。
- 输入是 display-referred RGBA8 FinalLdrColor；本节记录的初版 shader 使用 FXAA 3.11 风格 luma 方向滤波，已由 2026-09-01 的 Quality 修复取代。输入格式、UV clamp 与按 viewport 上传 inverse texel size 的契约继续保留。
- `PresentSource` 每帧先回退到 FinalLdrColor。FXAA 只有在 geometry/program/binding/FBO/attachment 全部有效并成功 submit 后才标记 FxaaColor 和提升 semantic；任何失败都由 Present 显示原 FinalLdrColor。
- 新增 `Renderer::setFxaaEnabled/fxaaEnabled`，默认开启且跨 pipeline rebuild 保持。关闭时 FrameGraph 不声明 FxaaColor，不分配目标，也不执行 FXAA draw。
- `AYRenderer_FXAA` 37/37 通过，覆盖 ABI、四条产品管线顺序、view order、runtime toggle、custom descriptor 兼容、production latch、Noop 与真实 Phoskia/shaderc 编译反射；全量 `AYRenderer_Test` 3525/3525 通过。

### 2026-08-30 — FinalLdrColor / Present 边界与后处理扩展基线

- PostProcess 从“最终 backbuffer blit”改为 FrameGraph `FinalLdrColor` 生产者：view 15 在 viewport-local RGBA8 目标完成 bloom、exposure、tone-map 与 gamma，并仅在真实 submit 后发布 current-frame production latch。
- 新增 append-only `RenderPassSlot::Present=15`、`FgResourceId::FinalLdrColor=6` 与 `FgSemantic::PresentSource=4`。旧 custom descriptor 若含 PostProcess 但没有 Present，会在配置时补入兼容边界。
- PresentPass 使用 view 16，只负责把本帧有效的 PresentSource 拷贝到默认 backbuffer 的 Game View rect。PostProcess 与 Present 共用 `FullscreenPassGeometry` 实现，资源仍由各 Pass 独立拥有和销毁。
- 默认/Deferred/Editor 管线均固定为 PostProcess → FXAA → Present；EditorOverlay 位于 Present 后、UI 前，view 251 负责方向轴，view 252 后续启用为 Transform Gizmo。选中轮廓在 Transparent 阶段使用 view 244/253/254：244 以未 jitter 投影写稳定 Alpha silhouette，253 以 jitter 投影和场景深度写 RGB visibility，254 在 Present 后按各自栅格取样并重建固定两像素外边界，避免选中框进入 TAA history 或随 jitter 覆盖率抖动。显式 view order 同时避开 FXAA 17、Shadow 18–25、UI Layer 26–243、TAA diagnostics 245 与 GBufferDebug 250。
- FrameGraph 新增通用 `markProduced/producedThisFrame` latch 与 semantic 级查询，`beginFrame` 与 shutdown 清零，防止复用物理 handle 时 Present 读取上一帧目标。测试覆盖 ABI、管线顺序、view 区间、Noop、latch reset 和生产 Present shader 编译。
- MSVC Debug 定向重编后 `AYRenderer_Test` 为 3429/3429；`AYEditorShell_Demo` 完成重新链接。为规避历史 stale `.obj` 问题，本轮只清理了 `AYRenderer_Test` 对象目录，没有执行全引擎 clean。
- 该边界为 FXAA、ColorGrading 等后 tone-map Pass 提供稳定插入点；TAA/MotionBlur/DOF 仍需先完成 motion/history/depth 契约，不在本刀混入。

### 2026-08-29 — Production UI Layer 与共享 RenderTargetPool

- Renderer 新增唯一的 renderer-wide RenderTargetPool；FrameGraph 和 UIRenderBackend 共用，按精确
  descriptor 复用 generation lease，并提供两帧 quarantine、LRU best-effort 预算、reset/shutdown。
- Pool 新增 strict acquire/预算 miss/peak bytes；FrameGraph 保持 soft acquire，UI target 不能新增
  超预算 storage。UI Layer backing 改为懒申请，压力下撤销 LRU backing、保持逻辑 handle 并同帧降级。
- FrameGraph 的 owned target 从直接创建/销毁迁移为 pool lease，standalone 测试仍可使用内部 owned pool。
- UIRenderBackend 完成 RenderTarget 和 Layer 全生命周期、纹理/blit、透明/color/preserve clear；
  当前 offscreen view 为 26–243，主 view 255，并保持 target transition batch barrier 与状态恢复。
- AYUI root 主树可 opt-in retained pixel layer；clean frame 单 composite，overlay/drag visual 即时叠加，
  显式 dirty rect 局部清除/replay，无范围 dirty、resize/device reset 全量重绘，能力或 paint 失败同帧回退。
- AYUI damage 扩展为最多 8 region 与 70% full 阈值，并支持 Always/Auto subtree Layer；backend 暴露
  repaint/cache/pool/pressure 统计和可调 UI budget。
- Noop 回归覆盖复杂 gradients/SDF/nine-patch/vector fill/stroke/nested path clip、pool 回收和
  partial damage、strict budget/LRU 压力、224 次离屏 pass 溢出恢复、resize/MSAA reset；当前 Windows
  Debug 全量 `3334 / 3334`。
- UI 目标的 point/linear sampling 纳入 pool 精确键；保留四参 framebuffer 入口并增加五参重载，
  避免增量对象 ABI 断裂。Layer 改用 coverage-correct alpha 与 premultiplied composite，gradient clip
  按原 bounds 重映射颜色。
- Auto 与显式 D3D11 在 1.0×/1.5× 下完成 10 场景真实纹理矩阵：immediate 30、full Layer 31、
  clean Layer 1、partial Layer 11 draw calls；语义差异最多 1 LSB，clean reuse 字节精确。
- D3D11 图像门禁扩展为 36 次 capture，新增透明 Layer、group/nested opacity、三种高级 blend、
  resize/动态 DPI、device/MSAA reset lease recovery 与 Transparent/Color/Preserve partial clear。
  该矩阵定位并修复小数 Layer origin 像素中心错位、高级 blend alpha 方程错误和半透明 Color clear
  未 premultiply 三项真实 GPU 缺陷。D3D12/Vulkan/OpenGL 随后通过同矩阵；跨后端验证另外定位并
  修复 D3D12 shaderc profile、capture DPI/真实 HWND resize、OpenGL RT V 翻转及 glyph 半像素覆盖。

### 2026-08-28 — GBuffer v3 冻结，进入 LightingPass 审核

- GBuffer 附件和打包索引收敛到 `GBufferLayout`，颜色 MRT 保持 160 bpp。
- 增加 `producedThisFrame()` 消费契约，防止冷启动/缩放/失败帧误读旧附件。
- StandardLit/Unlit 通过 RT2.a 与 AO 打包，RT3.a 独立保留 Geometry Coverage。
- 保留 RT2 WorldPosition；在跨后端 GPU 验证前不启用 Depth 重建。
- 当时暂不新增 Velocity RT；该决策已由 2026-09-01 的独立、按需 `MotionVectorPass` 取代，但 GBuffer MRT 本身仍保持 160 bpp。

### 2026-08-28 — LightingPass 第一轮审核

- 审核时结构是一个 view 8 的全屏光照 Pass：消费四个 GBuffer 颜色附件、Skybox/环境立方体、`SceneLights` 与 `ShadowPass`，当时输出独立 `RGBA8` LightingOutput；后续第二轮已升级为 `RGBA16F`，2026-08-29 又将 SSAO 调整为 Lighting 的前置 ambient 输入。
- 正向项：`GBufferPass::producedThisFrame()` 输入门禁、Lighting 自身逐帧产出门禁、冷启动/缩放前置 FBO 准备和资源销毁路径已经闭合。
- 阻断项：每光源 shadow atlas 复用同一 bgfx View 修改 view transform/scissor，且上传给 Lighting 的数组保存的是 View 而非 ViewProjection；Spot caster 还读取了 `Light::direction` 而不是 `spotDirection`。
- 高优先级项：多光源 PBR 只有槽 0 计算镜面项；阴影 shader 每像素静态执行 81 次采样；atlas 投影范围与 PCF tile 边界不严；未使用槽位会计算等边界 `smoothstep`；LightingOutput 为 LDR，曝光/tonemap 前已截断高光。
- 验证现状：B5/B5.5/B7 三组 220/220 断言通过，但本次运行未配置 shaderc，测试允许 program acquire 失败，且大多只验证字符串/契约；不能替代 D3D11/12 实机编译与 GPU capture。

### 2026-08-28 — LightingPass 第二轮按序修复

- Shadow atlas 生产端使用 view 1 整图清理、槽位独立 view 18–25 按 tile viewport/scissor 写入，并通过每帧 view order 保证其先于 resolve 与 Lighting；每槽分别保存 View、Projection 和 ViewProjection，Spot caster 使用 `spotDirection`。
- Shadow 只在当前帧实际生产并 resolve 后可采样；现代 `SceneLights` 无受支持 caster 时不回退旧关键光，legacy 单光路径统一发布为槽 0 全图采样契约。
- Lighting 上传精确 `activeLightCount`，未使用灯槽写入有限中性值，输入 light 参数做非有限值和范围清洗；8 个灯槽均执行完整 Cook-Torrance GGX，而非仅槽 0 有镜面项。
- Shader 按 shadow caster 数量与 PCF 开关生成变体：无阴影 0 次 shadow sample，硬阴影每槽 1 次，PCF 每槽 9 次；删除重复 legacy 关键光采样。
- Atlas 投影增加 clip.w、局部 UV 与参考深度门禁，PCF 中心限制在 tile 内侧 1.5 texel，避免跨 tile 污染；采样 texel 使用实际 4096 atlas 尺寸。
- LightingOutput、BloomBright、BloomBlurA/B 与 HazeColor 统一为共享 `RGBA16F` HDR 中间格式，最终 tone map/gamma 仍由 PostProcess 执行。
- 新增 `AYRenderer_LightingAuditRound2` 回归组，覆盖 HDR 格式、view 保留区、采样次数、无阴影公共计算裁剪和四类实际 Phoskia 变体 IR；定向 Lighting/Skybox 旧缓存键断言已同步。
- MSVC Debug 构建通过；`AYRenderer_Test` 全量 2936/2936 通过。该结果仍不替代真实 D3D11/12 shader 编译与 RenderDoc capture。
- 尚未关闭的生产门禁：真实 D3D11/12 shader 编译与 RenderDoc capture；Point 光全向阴影仍不支持，Spot caster 暂用 scene-fit 正交投影而非透视锥体。

### 2026-08-28 — TransparentPass 第二轮按序修复

- Forward 路径继续写入 scene FBO；Deferred 路径在 view 9 借用 LightingOutput 颜色附件和 GBuffer 深度附件组成缓存 FBO，透明对象只读深度、不写深度。GBuffer/Lighting 任一未在当前帧有效生产时直接 fail-close，不再退化到错误目标。
- 排序契约固定为显式 `sortKey` 降序优先、相机距离平方降序次优先、完全相等时保持输入顺序，并使用 `Sequential` view mode 保证 CPU 提交顺序不被重排。
- 抽取共享 `SceneLighting` 打包/上传路径供 Lighting 与 Transparent 使用；内建透明 PBR 支持 8 个 Directional/Point/Spot 灯完整 Cook-Torrance BRDF、caster-first 每光源 shadow atlas、tile 安全采样与环境立方体 IBL。
- 修正混合状态：straight 与 premultiplied alpha 均使用独立 alpha 通道 `ONE / INV_SRC_ALPHA`，additive 通过 `ZERO / ONE` 保留目标 alpha；所有透明模式统一 `LEQUAL` 且禁用深度写入。
- Transparent 借用 FBO 按颜色/深度句柄缓存，并在管线重配、resize、MSAA 修改与 shutdown 前主动销毁，避免持有已失效的生产者附件。
- 内建 PBR 已通过 Phoskia IR、Linux GLSL 430 与 Windows s_5_0 编译检查；新增/更新 Transparent、PBR 与共享灯光测试，相关回归均通过，手动使用当前 Ninja/MSVC 参数链接的 `AYRenderer_Test` 全量为 2987/2987。
- 尚未关闭的生产门禁：真实 D3D11/12 RenderDoc capture；传统 alpha 仍依赖排序，不提供 OIT；透明物体不写 GBuffer，因此 SSAO、轮廓和其它只消费 opaque GBuffer 的效果仍按 opaque 深度语义工作；DepthHaze 对内建透明 PBR 单独按 fragment world position 处理，自定义透明 shader 需要声明可选 haze uniforms；Point 全向阴影和 Spot 透视锥体阴影沿用 LightingPass 的后续能力项。

### 2026-08-28 — BloomExtract/BloomBlur 两轮审核与按序修复

- 架构固定为三个 FrameGraph 半分辨率瞬态目标：`BloomBright`、`BloomBlurA`、`BloomBlurB`。Pass 自身仅持有全屏几何、shader 和临时 attachment handle；A/B 的生命周期重叠，禁止物理 alias。view 顺序为 Extract 10、BlurH 11、BlurV 12，PostProcess 仍在 15。
- 数据流为 `Forward sceneFbo / Deferred LightingOutput → BloomExtract → BloomBright → BlurH → BloomBlurA → BlurV → BloomBlurB → PostProcess`。Forward scene FBO 与 Deferred LightingOutput 均优先申请共享 `RGBA16F` HDR 格式；不支持时由 BGFXAdapter 按候选格式安全降级。
- Extract 在 scene-linear HDR 上执行中心加四角共 5 次采样的 Karis 权重降采样，再以 `threshold` 与 fractional `softKnee` 提取高亮；Blur 使用线性过滤折叠的高斯核，每轴 5 fetch，替代原先每轴 9 fetch。
- Bloom 被视为单一完整能力：strength 必须为有限正数，且 Extract/Blur 均存在并启用，才声明三个 FG 资源；任一阶段缺失或禁用时零分配。strength、threshold、soft-knee 对负数、NaN、Inf 做统一清洗。
- Extract 与 Blur 每帧先清除 `producedThisFrame`，仅在真实 submit 后置位。Final 只有在 Blur 本帧生产且 `BloomSource`/attachment 均有效时才保留非零 strength；fallback sampler 仍绑定合法纹理，但 strength 强制为 0，避免旧帧采样和 `raw + scene * strength` 双亮。
- Bloom 与 scene color 在曝光前保持同一线性空间，Final 主 shader 使用 `(scene + bloom * strength) * exposure` 的等价组合后再 tone map/gamma；独立最小 fallback 只做 sceneColor blit，不依赖 bloom、曝光或 tone-map 语法。
- 测试直接检查生产 shader 字符串，不再维护易漂移的测试镜像；覆盖完整链 truth table、逐帧 fail-close、参数边界、HDR 格式、采样数/offset、binding 与曝光合成。精确生产源已手动通过 Phoskia IR、Linux GLSL 430 和 Windows s_5_0 检查；稳定全量 `AYRenderer_Test` 为 3022/3022。
- 可选后续优化按收益排序：先做真实 D3D11/12 capture 和阈值/曝光标定；需要更宽光晕时升级为多级 downsample/upsample 金字塔；再考虑 quarter-res 质量档、lens dirt、anamorphic streak 或 temporal stabilization。现阶段不引入多级链，避免在没有 GPU 基线前增加 RT、带宽与调参维度。
- 仍未关闭的生产门禁：真实 GPU capture、边缘采样/resize 观测、RGBA16F fallback 后的视觉降级验证；当前为单层半分辨率 Bloom，不具备多尺度光晕。

### 2026-08-28 — DepthHaze 两轮审核与按序修复

- 架构收敛为 FrameGraph 所有的全分辨率 `RGBA16F HazeColor`；DepthHazePass 自身只持有全屏几何、shader/binding 和 `producedThisFrame` latch。原 `HazeHalf` 数值 ID 4 保留为源码兼容别名，新代码统一使用 `HazeColor`。
- Deferred 数据流固定为 `GBuffer → optional SSAOTexture → LightingOutput + RT2 WorldPosition + RT3 GeometryCoverage → HazeColor → Transparent → Bloom → PostProcess`。显式 bgfx view order 保持稳定 ID 的同时强制 `SSAO(14) → Lighting(8) → Haze(13) → Transparent(9) → Bloom(10–12) → PostProcess(15)`。
- RT3.a 明确表示 geometry coverage：有几何像素按相机到 world position 的指数距离雾计算；无几何背景按无穷远策略接收完整请求强度，不再把清屏 world position 误判为近距离表面。
- SSAO 只在本帧实际产出且 semantic/attachment 有效时由 Lighting 消费，并仅衰减 StandardLit 的环境/IBL 项；DepthHaze 与 PostProcess 均不再读取 SSAO，因此 direct、emissive、unlit 与雾散射不会被整屏乘暗。
- PostProcess 删除独立 `hazeTexture` 与二次 haze composite，直接把本帧有效的 HazeColor 提升为主 scene source；Bloom 从同一 HDR source 提取。DepthHaze shader、PostProcess 主/回退 shader 均直接通过 Phoskia 与 Linux GLSL 430 编译验证。
- Transparent 在 opaque haze 后写回同一 HazeColor 并借用 GBuffer depth；内建 PBR 用 fragment world position 计算透明雾。Alpha 路径注入雾色，Additive 路径只按透射率衰减，避免凭空增加能量；ForwardOpaque 每次清零可选 haze uniforms，防止共享 program 状态串帧。
- strength/density/color 统一清洗 NaN、Inf 与负值；Pass/producer/backend/viewport 任一缺失时不声明资源或不提交。GBuffer、Lighting、SSAO、Haze 与 Bloom 的逐帧 latch 在图构建前统一复位，消费者不再把“句柄仍有效”误当成“本帧数据有效”。
- MSVC Debug 干净构建通过；四组 DepthHaze 定向契约 131/131，通过真实 shader 编译与 view-order 检查后，`AYRenderer_Test` 在同一代码上连续三次 3052/3052 全绿，未再出现断言窗或挂起。
- 可选优化顺序：先以 D3D11/12 capture 标定天空、地平线、轮廓边缘和透明叠层；确认带宽成为瓶颈后再评估 half/quarter-res + 双边上采样；待 GBuffer 深度重建方案成熟后可移除 RT2 WorldPosition；只有确有体积光需求时再升级 froxel/temporal volumetric fog。
- 剩余限制：自定义透明 shader 若不声明可选 haze uniforms 将保持未雾化；当前是解析式距离雾，不支持局部体积、光束或时间积累；全分辨率 HazeColor 增加一轮 HDR 读写，仍需真实 GPU capture 定量验收。

### 2026-08-29 — SSAOPass 审核与按序修复

- 架构固定为 Deferred-only 的 `GBuffer → SSAO → Lighting`：SSAO view 14 读取 RT2 world position、RT1 encoded normal 与 RT3.a geometry coverage，写入 FrameGraph 所有的全分辨率 `RGBA8 SSAOTexture`；R 保存遮蔽率，A 保存中心 coverage。Lighting view 8 是唯一消费者。
- 修正 coverage 来源：不再把 RT2.a 的 material AO/model 打包值当作几何有效位；中心与每个 kernel tap 均读取 RT3.a，天空/越界采样 fail-close。
- 8-tap kernel 以 GBuffer normal 构造 TBN，并按像素哈希旋转；样本从近到远分布于法线半球。遮挡比较使用符号无关的 `abs(view-space Z)`，bias 为固定视空间单位，不再比较相机径向距离或按距离放大 bias。
- 参数入口和逐帧广播统一清洗：strength 限制到 `[0,1]`，radius/bias 拒绝负值与 NaN/Inf，逐帧 bias 再限制到 radius。完整 gate 要求 SSAO/GBuffer/Lighting 三个 Pass 均存在且启用、后端与 viewport 有效、strength/radius 非零；否则不声明 SSAOTexture。
- 合成所有权收敛到 Lighting：coverage-aware 五点过滤后，AO 只乘 StandardLit ambient/IBL；direct、emissive 与 Unlit 保持不变。DepthHaze/PostProcess 删除 SSAO sampler、uniform 与重复滤波，避免整屏乘暗和有雾/无雾路径不一致。
- `producedThisFrame` 在每帧和每次 execute 起始清零，仅真实 submit 后置位。管线重配 `pipeline.clear()` 前和 Renderer shutdown/adapter teardown 前都显式销毁 SSAO 全屏 VB/IB 与 program/binding，关闭重启后的 stale handle/ABI 断言窗口。
- D3D11 复核发现关闭 SSAO 时，中央 FrameGraph gate 会正确省略 `SSAOTexture`，但 Deferred 管线仍会调度已挂载的 SSAOPass，旧实现先 resolve 目标再检查完整链状态，从而持续误报 `SSAOTexture resolve invalid`。现由 FrameGraph 与 Pass 共享完整 `selectSsaoStage()`：关闭/零强度/零半径、GBuffer/Lighting 缺失或禁用、零视口均在 resolve 前静默返回；开启后日志已确认 `SSAOPass first dispatch`，目标分配本身正常。
- 测试改为直接消费生产 shader：覆盖参数非有限值、完整链 truth table、RT3 coverage、TBN、view-Z、ambient-only 所有权、Haze/PP 无重复 SSAO、cache key、latch 与 teardown 顺序，并实际执行 Phoskia + Linux GLSL 430 编译；不再用 `CHECK(true)` 或测试内复制 gate 形成假绿。
- 修改 `PostProcessPass` 私有布局后的增量产物曾在 `FinalPPPass_S1c` 触发 `Stack around the variable 'pass' was corrupted`；同一配置完整 clean rebuild 后该套件 46/46 正常退出，确认是新旧 `.obj` 混用导致的 ABI 污染，不是 SSAO shader 越界。随后 SSAO 161/161、Lighting 32/32、DepthHaze 64/64 定向回归通过，最终 MSVC Debug 全量 `AYRenderer_Test` 为 3095/3095，未再出现断言窗或异常退出。
- 暂不实施的可选优化：half-resolution + depth/normal-aware bilateral upsample、`R8` 单通道目标、由 depth 重建 view position、蓝噪声/temporal accumulation 与 GTAO。先用 D3D11/12 capture 测量 SSAO 带宽、边缘 halo、噪声和参数尺度，再按收益选择，避免在无 GPU 基线时同时改变格式、分辨率和算法。

### 2026-08-29 — PostProcessPass 审核与正确性收敛

- 截至该轮，PostProcess 已固定为最终全屏 composite 且不拥有私有 FBO：输入由共享 `SceneColorPipeline` 选择 Forward `sceneFbo`、Deferred `LightingOutput` 或本帧有效的 `HazeColor`。当时输出仍直接写默认 backbuffer；该输出边界已在 2026-08-30 被上方 FinalLdrColor/Present 架构取代。
- `SceneColorPipeline` 成为 Haze、Transparent、Bloom 与 PostProcess 的共同路由所有者；`PostProcessPass::selectSourceFbo` 只保留兼容转发，避免其它 Pass 依赖最终合成类的私有策略。
- Pass 本地生命周期仅包含 fullscreen VB/IB、ShaderResource 与 binding。Renderer 在 `applyPipelineDesc()` 的 `pipeline.clear()` 前以及 shutdown 的 shader pool/adapter teardown 前显式调用 `destroyResources()`，关闭重配和 shutdown→initialize 的 stale numeric handle 路径。
- 主 Shader 删除未消费的 `uTime` ABI。Exposure 在 `[0,64]`、Gamma 在 `[0.1,8]` 内清洗；NaN/Inf 使用中性默认值，并在公开 setter、FrameContext 广播和 Pass 上传三层 fail-safe。
- 原先与主 Shader 几乎同构的 fallback 改为只声明 `sceneColor` 的独立最小 blit；fallback 可见时仍每 120 帧尝试恢复主程序，两者都失败时也不会逐帧调用 shaderc。
- ForwardOpaque 在设置 scene clear 后显式 `touch(viewId)`，保证场景列表非空但没有有效 opaque submit 时仍写入本帧清屏色，PostProcess 不再采样上一帧 scene FBO。
- `PostProcessPass` 构造/析构改为 `.cpp` 唯一定义，避免 Visual Studio 增量构建从旧测试对象选择按历史类尺寸生成的 inline COMDAT；本轮已用链接器符号定位并定点重编相关 AYRenderer 测试对象，没有执行全引擎 clean。ForwardOpaque 的 initialized gate 同时前移到 `setViewMode` 之前，修复未初始化 bgfx 下的旧 R5Plus pipeline 挂起。
- 测试不再维护 PostProcess Shader 的本地镜像或用故意编译失败代替生产编译；主/回退源码直接从运行时代码导出，覆盖 binding、cache key、参数边界、共享路由、bounded retry、teardown 顺序与 Forward touch。
- PostProcess 定向回归全部通过：R51 Smoke 2/2、R51 62/62、FinalPP S1c 54/54、P0 13/13、F5 27/27、R5Plus 19/19、B6 SourceFbo 9/9，并通过相关 Bloom、DepthHaze、SSAO 与 AuditP5 回归。最初 3 个 UI Layer 容量断言把“每帧 224 个 offscreen pass”误写成“同时持有 225 个共享 framebuffer”；改为在同一有效 Layer 上隔离验证 view 分配并锁定 26/249/224 常量后，MSVC Debug 全量为 `3211 / 3211`。
- 暂不加入 dithering、精确 sRGB OETF、HDR swapchain 输出、额外 tone-map 算法或多级 bloom；先完成 D3D11/12 真机 Shader 编译与 RenderDoc capture，再按 banding、色彩管理和性能数据决定。

### 2026-08-30 — LUT ColorGrading Pass

- 新增 append-only `RenderPassSlot::ColorGrading=17` 与 `FgResourceId::ColorGradedColor=8`，固定在 `PostProcess → FXAA → ColorGrading → Present`。复用已释放的 legacy view 4，并通过显式 `RenderViewOrder` 排在 view 17 与 view 16 之间，不侵占 UI 26–255 区间。
- LUT 使用 portable `texture2d`：32³ RGB cube 按 blue slice 横向展开为 `1024×32 RGBA8`，R/G 由硬件双线性过滤，B 在 shader 中对相邻 slice 显式插值。内置 Neutral/Warm/Cool/Cinematic 四种程序化预设；Renderer 默认关闭、Warm、0.75，强度和非法枚举均在公开入口清洗。
- FrameGraph 只在 enabled、非 Neutral 且强度大于零时声明 ColorGradedColor。Pass 读取当前 `PresentSource`，因此 FXAA 关闭/失败会自然回退 FinalLdrColor；仅在 LUT、程序、binding、geometry 与 submit 全部成功后标记本帧产物并提升 semantic。
- LUT 仅在预设变化时重建；fullscreen geometry、LUT texture、program/binding 在管线重建和 shutdown 的 adapter teardown 前显式销毁，shader 获取失败使用 120 帧有界重试。当前未提供外部 `.cube`/图片 LUT 资源导入。
- 视觉验证修复将 Neutral 明确标为 bypass；Editor 从 Neutral 开启时自动提升到 Warm，并迁移历史 `enabled + Neutral` 偏好。Pass 增加首次成功 dispatch 记录，并把 PresentSource 与 ColorGradedColor resolve 失败拆成独立诊断。
- 真实 D3D11 日志曾显示 SSAOTexture 与 ColorGradedColor 同时 resolve 失败。共享 RenderTargetPool 现于底层 framebuffer 创建失败时，只淘汰一个已越过两帧 quarantine 的 LRU 空闲目标并重试一次；不会回收 active lease，也不会破坏 in-flight 安全边界。定向重编 stale `FgResource.cpp.obj` 与 `RenderPipeline.cpp.obj` 后 ColorGradedColor 分配恢复；SSAO 报告随后确认是关闭状态的调度误报，开启时目标可正常分配和 dispatch。
- D3D11 随后暴露出 Linux/GLSL 门禁未覆盖的 shader 错误：Phoskia HLSL emitter 将 `clamp(vec3, vec3, vec3)` 结果错误推断成标量，使 `scaled.z` 在 `s_5_0` 编译失败。LUT 坐标现拆为 `scaledX/Y/Z` 标量，cache key 提升为 `color_grading_lut2d_32_v2`，并新增 Windows `s_5_0` 实编译回归。
- `Test_ColorGrading` 覆盖 ABI、产品管线/view 顺序、参数持久化与清洗、自定义管线兼容、LUT 布局、semantic 成功后提升、Noop/双重销毁、GLSL 与 D3D `s_5_0` 生产 Phoskia 编译及 binding 反射；定向 55/55，MSVC Debug `AYRenderer_Test` 全量 3623/3623。
- 增量构建先后捕获到 `AYRenderer.cpp.obj`、`FgResource.cpp.obj` 与 `RenderPipeline.cpp.obj` 早于对应私有头，旧对象混用还会触发测试栈损坏。最终只清理并重建 `AYRenderer` 库及 `AYRenderer_Test` 对象目录，未清全引擎；全量回归不再出现内存写入断言。新 Demo 运行日志已确认 `[ColorGradingPass] first dispatch ... preset=3 strength=0.74`。

### 2026-07 — 引擎闭环（R4 + Engine）

- `RendererSubSystem`：GameLoop 子系统，单线程 `renderFrame` callback。
- `RenderSystem`（AYEntity）：ECS → `RenderScene` → `ForwardOpaquePass`。
- Demo：`AYEngineIntegration_Demo` 验证全链路。

### Bootstrap / Shutdown

| API | 说明 |
|-----|------|
| `setBootstrapWindow` | Win32 HWND + 分辨率（`initialize` 前） |
| `setBootstrapShaderDumpDirectory` | Phoskia 中间产物 dump（`initialize` 后写入 pool） |
| `RendererSubSystem::shutdown` | 先 `setRenderCallback({})`，再 `Renderer::shutdown()` |
| `GameLoop::shutdown` | Demo 退出前调用，逆序关闭子系统 |

### AYShader 协作修复

- `ShaderResourcePool::Impl` 的全局 pool registry 改为 **intentionally-leaked** 堆 map，
  修复进程退出时 `unregisterPool` 访问已析构 static map 的 AV（C0000005）。

### 依赖模块同步

- **AYEntity**：`bootstrapModule()`、`SparseSet` 指针语义（见 AYEntity `design.md` §15）。
- **AYCore**：`AYCore/CoreSerializer.h` 提升属性宏。
- **AYSerializer**：`SerializerFor<T,void>` 默认 reflect 路由（见 AYSerializer README §变更记录）。

### 下一步

TAA 当前契约以本页 2026-09-21 基线和 2026-09-23 增量记录为准：固定输出网格、RGBA16F motion/previous-depth/reactive 打包、3×3 最近有效速度 dilation、逐 tap 历史验证和失败帧联动失效。下一步应在 D3D11 Editor 验证静止收敛、相机运动、动态角色、对象生成/删除、Edit/Play 切换、resize/camera-cut 与透明物体边界，并补齐 WorldLit2D 的运动重放；Motion Blur 与多层透明/折射的专用时域契约仍需独立设计。
