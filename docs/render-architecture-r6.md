# AYRenderer R6 架构收口计划

> 日期：2026-09-09
> 状态：R6-0、R6-1、R6-2、R6-3、R6-4 已落地；R6-5 第一阶段已落地；R6-6 已完成。
> 范围：在不重写 bgfx/RHI、不改变 Forward/Deferred 画面顺序的前提下，收口 Pass 描述、资源依赖、诊断和场景提交。

## 1. 当前真实基线

当前功能链已经覆盖 Shadow、GBuffer、MotionVector、SSAO、Lighting、DepthHaze、Transparent、Camera Overlay 2D、Bloom、PostProcess、TAA/FXAA/SMAA、ColorGrading、Present、Editor Overlay 与 UI。继续直接增加效果会放大以下结构问题：

- `RenderPipeline` 仍按注册顺序执行 `RenderPass`，但带 slot 的 Pass 已由编译后的 `FrameGraph` live decision 控制是否 dispatch；资源黑板已覆盖时域历史、Deferred 主链与 Bloom 链的可消费输出。
- `Renderer::render()` 同时负责 Pass 查找、状态预备、效果门禁、资源声明、semantic 选择和执行上下文拼装。
- `PassExecContext` 通过多个具体 Pass 指针传递生产者状态，新增效果容易继续增加横向耦合。
- 几何 Pass 已复用每帧一次生成的共享 DrawList 做领域分桶与稳定排序，已启用可靠静态 bounds 的保守视锥剔除；状态排序合批与 instancing 尚未启用。
- 自动化测试已覆盖契约与 shader 编译，但真实 D3D11 capture、GPU 时间和资源带宽基线仍不完整；D3D12 完整恢复可延后，但必须保持可诊断、失败不闪退。

因此 R6 期间默认不新增独立画质 Pass。例外仅限修复现有 Pass、诊断视图和为架构收口服务的内部节点。

## 2. 目标边界

R6 完成后，增加一个普通后处理节点应只需要：

1. 新增 Pass 实现及其静态资源契约；
2. 在管线计划中注册节点和产品开关；
3. 增加 shader、Noop、resize、teardown 与真 GPU 验证。

不应再要求在 `Renderer::render()` 中复制资源描述、手工查找多个具体 Pass、拼接嵌套输入选择或依赖“有效 handle 等于本帧已生产”。

R6 明确不做：

- Vulkan/D3D12 风格命令缓冲重写；
- 替换 bgfx 或建立第二套 RHI；
- 自动 barrier/多队列调度；
- 未经 capture 数据驱动的激进 RenderTarget alias；
- 一次性迁移所有 Shadow/GBuffer/Lighting 生命周期。

## 3. 分刀顺序

### R6-0：文档与基线收敛（已完成）

- 本文成为当前架构开工入口；旧的 `short-term-plan.md` 和 `frame-graph-mvp.md` 保留为历史 cutsheet。
- `renderer-pass-roadmap.md` 与引擎级 Rendering Roadmap 对齐 MotionVector/TAA、FrameGraph 和 Camera Overlay 2D 现状。

### R6-1：FrameGraph 编译契约（已完成）

- 校验 enabled pass 的未声明 read/write。
- owned resource 必须先生产后读取；同一 logical resource 禁止多写者。
- semantic 不得指向未声明或无生产者的 owned resource。
- 存在 Present/Consumer 等终端节点时，从终端与 semantic 输出反向标记 live pass，裁剪断开的 enabled 分支。
- semantic 同时承担图外消费者根：例如 SSAO 由图外 Lighting 消费。
- imported persistent target 优先依赖本帧较早写者；只有不存在本帧写者时才作为外部输入，覆盖 TAA history 的正确时序。
- compile 失败输出结构化诊断并让 owned 输出 fail-close；不改变 `RenderPipeline` 的执行顺序。

### R6-2：图计划从 `Renderer::render()` 外移（已完成）

- 新增纯内部 `PostProcessGraphPlan`；输入只包含尺寸、外部 scene/TAA handle 和已经解析的阶段布尔值，不持有任何具体 Pass。
- SSAO、Haze、Bloom、FinalLdr、TAA/FXAA/SMAA、ColorGrading 与 Present 的 logical resource、读写边和 semantic 声明集中在 plan builder。
- AA 选择保持既有优先级 `TAA > SMAA > FXAA > FinalLdr`；异常多开组合仍确定性编译，未接入 Present 的低优先级分支由 liveness 裁掉。
- `Renderer::render()` 只保留具体 Pass/backend 能力判断、计划输入组装、执行上下文和 dispatch；cache key、view id 与 `RenderPipeline` 顺序未改变。
- 新增 5 个计划组合测试（32 项断言），覆盖空图、完整 temporal 链、FXAA→SMAA、TAA 优先裁枝和不完整 Bloom fail-close。

验收：后处理图声明已从 `Renderer::render()` 移除；定向 FrameGraph/后处理/AA 回归通过，全量 `AYRenderer_Test` 为 4119/4119，`AYRenderer_Demo` 链接通过。R6-2 当时被 AYEditor 并行编译错误阻断的 `AYEditorShell_Demo` 门禁已在 R6-3a 补跑通过。真 GPU 画面仍归 R6-6 capture 门禁。

### R6-3：Pass 契约与类型安全注册（已完成）

- **R6-3a（已完成）**：`RenderPipeline` 在 `addPass/clear` 时维护 exact-type 索引，生产路径改用 O(1) `findPass<ConcretePass>()`；移除 `AYRenderer.cpp` 中字符串查找后直接 `static_cast` 的组合。诊断名称仍可重复，重复具体类型保持“第一次注册优先”。
- **R6-3b（已完成）**：为 21 个公开 Pass slot 建立静态 `reads/writes/output format/extent/lifetime/side-effect` 契约；管线重建前验证未知/重复 slot 和 required resource 的缺失、错序，失败时保留上一条有效管线。后处理图的 transient RT 格式与尺寸直接从同一契约生成，动态输入选择仍由 `PostProcessGraphPlan` 决定。
- **R6-3c（已完成）**：挂载由 slot 注册表示，启用由 `RenderPass::isEnabled()` 表示，本帧资源生产继续由 FrameGraph production latch 表示；编译后的 slot liveness 直接约束 dispatch。`RenderPipeline` 为每个已挂载 Pass 保留只读执行结果，明确区分 `Submitted`、`CompletedNoDraws`、`Disabled`、`GraphCulled` 与 `Failed`，不再用返回值 0 同时表达所有情况。

验收：缺 Pass、错顺序和错资源在 plan compile 阶段报告，不等到 GPU submit 才表现为黑屏。

R6-3a 验证：类型查找、重复注册、`clear()` 失效和 const lookup 共 11 项断言通过。

R6-3b 验证：21 个 slot 契约覆盖、四条 canonical pipeline、资源 lifetime、缺生产者、错序、重复/未知 slot、TAA/Overlay 前置条件和“拒绝后不破坏基线”共 96 项断言通过；FrameGraph/PostProcessGraphPlan/PipelineConfig 定向回归通过；全量 `AYRenderer_Test` 为 4226/4226。

R6-3c 验证：FrameGraph 可裁剪 slot 不进入 `execute()`；禁用、正常提交、正常零绘制、图裁剪与异常五种状态均有定向覆盖。TAA 图声明补齐 GBuffer、MotionVector 与双 history 读写，避免执行依赖存在而图依赖缺失。

### 架构审核 5/6/7 收口（2026-09-21）

- **5 — 图编译与实际执行脱节**：带 slot 的 Pass 注册进入统一执行门禁，FrameGraph compile 的 liveness 结果会真实裁掉 dispatch；现阶段仍保留既有注册顺序，不声称已经实现自动调度或完整资源黑板。
- **6 — 关闭效果仍长期占用 RT**：FrameGraph owned resource 在连续 120 帧 inactive 后归还 pool lease；重新启用会重新申请并清零 idle 状态。窗口可配置，并提供 retained/released 统计用于后续诊断。
- **7 — 几何 Pass 重复扫描与零返回值歧义**：`FrameDrawLists` 每帧一次分类 opaque、GBuffer、transparent、WorldLit2D、Overlay2D、shadow caster/bounds 与 selection outline；透明和 Overlay 保持稳定排序。各几何 Pass 复用该结果，阴影多灯矩阵也不再逐灯扫描 `RenderScene`。Pass outcome 单独表达正常零绘制和失败。

验证：VS 2026 Insider x64 Debug 完整重编；`AYRenderer_Test` 全量 4603/4603。真实 D3D11 capture 与性能对比仍属于 R6-6。

### R6-4：资源黑板与历史资源（已完成）

- 用 renderer-internal resource blackboard 替代 `PassExecContext` 中可由 logical resource 表达的生产者指针。
- 明确 External、Transient、PersistentHistory 三类所有权。
- TAA history、MotionVector 与 resize/camera-cut/rebuild 失效规则集中记录；不强行迁移对象骨骼历史缓存。

第一刀完成项（2026-09-21）：

- 新增 renderer-owned `RenderResourceBlackboard`，登记 `MotionVectors`、`TaaHistoryRead`和 `TaaHistoryWrite`的 framebuffer/texture、尺寸、generation、lifetime、内容有效性与当帧产出状态。
- MotionVector 为当帧资源，跨帧自动回到 `AwaitingProducer`；TAA read/write 为 `PersistentHistory`，仅在显式失效时丢弃内容。
- resize、camera cut、pipeline rebuild、MSAA/backend reset、feature disable、prepare/producer failure 和 shutdown 都有结构化失效原因；TAA 不再只以“handle 有效”代替“内容可读”。
- 生产路径中 TAA 从黑板取双 history 与 motion texture，`PostProcessGraphPlan` 的对应 imported handle 也从黑板解析；直接构造 Pass 的旧单测仍保留 nullptr 兼容路径。
- 本刀黑板不销毁 GPU 资源，具体 Pass 仍是句柄 owner；这是有意的渐进边界，避免同时改变状态来源和所有权。

第二刀完成项（2026-09-21）：

- GBuffer 的 albedo、normal、world-position、material/coverage 和 depth 作为一个原子资源组发布；消费者只有在五个附件的 FBO、尺寸和 generation 完全一致且本帧已产出时才能获得视图。
- SSAO 和 DepthHaze 在成功 submit 后同时标记 FrameGraph production latch 并发布 `Transient` 黑板输出；跨帧自动失效，不再依赖保留 FBO handle 或具体 Pass 布尔值证明新鲜度。
- Lighting、MotionVector、TAA、DepthHaze、SSAO、Transparent、SceneColor routing 与 GBufferDebug 的生产路径已优先从黑板读取上述资源；只有不提供黑板的直接 Pass 单测使用旧指针兼容路径。
- pipeline rebuild、resize 和 backend reset 会联动失效所有已登记当帧输出。全量 `AYRenderer_Test` 为 4638/4638。

第三刀完成项（2026-09-21）：

- Lighting 与 Skybox 在成功 submit 后发布 pass-owned `External` color target，并以 generation 区分 resize/重建前后的物理资源。SceneColor routing 和 Transparent 不再以 LightingPass latch 判断画面新鲜度，Lighting 也不再以 SkyboxPass latch 判断 backdrop 是否可采样。
- BloomExtract 发布 `BloomBright`，BloomBlur 从黑板消费它并发布 `BloomBlurA/B`，PostProcess 直接消费 `BloomBlurB`。三个 FrameGraph resource 同时补齐 current-frame production latch。
- `lightingPass/skyboxPass` 指针仍承载 ambient strength、cube texture 等非纹理元数据；`bloomExtractPass/bloomBlurPass` 等旧字段仅保留给无黑板直接单测。中部聚合字段不物理删除，避免破坏旧 brace initializer 映射。
- 全量 `AYRenderer_Test` 为 4663/4663。R6-4 所定义的纯纹理 logical output 与 TAA history 迁移至此完成。

明确不并入 R6-4 的状态：Shadow 指针还承载光源矩阵、atlas rect 与 bias 元数据；Skybox/Lighting 指针的 cube/ambient 元数据也不是 logical texture output；对象骨骼历史继续留在 MotionVector 专用缓存。后续转入 R6-6 诊断与真 GPU 门禁。

验收：关闭、resize、管线切换和 shader 失败均不会让消费者读取 stale handle。

### R6-5：DrawListBuilder（静态剔除与刚性合批已完成）

- 每帧一次完成 opaque/cutout/transparent/shadow/overlay 分桶和稳定排序。
- 第一阶段已复用过滤结果，仍逐对象 submit；GBuffer、MotionVector、ForwardOpaque、Forward2DOpaque、Transparent 与 Shadow 共用同一份分类结果。
- 静态 3D frustum cull 已接入共享列表：使用有效 mesh local AABB、非抖动 VP、两像素边界；未知/无效 bounds、蒙皮、Morph、粒子、2D 和选择描边保守不剔除。Shadow caster/fitting 列表在相机剔除前构建，保持屏幕外投影。
- 第二阶段已对连续相同 mesh/material/range/state 的 rigid opaque GBuffer/solid Shadow
  启用 bgfx instancing；原有逐对象路径保留为关闭对照与安全回退。

验收：画面与 draw 顺序不变；CPU pass 时间下降；透明排序和 MotionVector stable id 不回归。

规模化第一刀（2026-10-03）：`setFrustumCullingEnabled` 默认开启，用于关闭对照；
`DrawItem::allowFrustumCulling=false` 用于未提供保守包围盒的自定义顶点位移。
公开帧统计记录 tested/culled/bypassed；此计数按 draw/submesh，不等于 Entity 数。
Contracts 122、Geometry 96、SceneMaterial 69、PostProcess 252 共 539 用例通过。
六面、near/w=0 交界、负/非均匀缩放、未知 bounds、固定 jitter 边界与重新入镜
Motion 历史都有回归；真实 GPU 图像/性能验收仍需独立执行，不能以 CPU 用例替代。

规模化第二刀（2026-10-03）：3D Scene 相机 provider 在几何提取前同步，优先级为
Host/Edit override > 活动 World 选主 > 默认镜头。World teardown 撤销 provider；
dispatch snapshot 在调用前后核对 registration，不能使用已撤销 World 回调。
相机来源/identity/cutGeneration 变化调用 notifyCameraCut，统一失效 TAA、Motion 和曝光历史。
组件/持久化/选主契约见 AYEntity README 与项目 SDK 3d.camera.view，不额外创建相机渲染 Pass。
VS 2026 Insider Debug：AYEntity_PerspectiveCameraTests 6/6、AYEntityTest 全量、
Renderer SceneMaterial 71/71、PostProcess 252/252 通过；SDK doctor 0 错误/警告。
这是 CPU/Noop 集成门禁，实际 Play 镜头切换和 GPU 历史 reset 仍需图像验收。

规模化第三刀（2026-10-03）：GBuffer 和 solid Shadow 通过 `RigidInstancing` 收集连续
兼容 draw，最多 256 个/批，再受后端 transient instance capacity 限制。只有足够容量、
有效实例 program 和成功绑定才前进多个 source item；编译失败锁存并回退，绝不丢对象。
三列 vec4（48 bytes）保留 affine linear + translation；VS 用 cofactor/determinant 构造
逆转置法线，兼容非均匀缩放与 shear。镜像绕序不同、退化/非有限/极端矩阵拆批/回退。
Shader 新增 `instance0..4` 的 vec4 vertex-only 契约（不占 fragment varying），仍由
Phoskia → IR → BGFX converter → shaderc → pool 编译，没有新 raw-SC 特例。
透明、cutout、蒙皮、Morph、2D、选择描边、线框、Forward custom material 不进入首版。
Motion 暂不合批，继续逐对象读取/提交原列表的 stable-id current/previous world/pose，
因此新增合批没有修改它的历史匹配或透明 reactive 覆盖顺序。
`setRigidInstancingEnabled(false)` 为关闭对照；公开统计记录实际 instancedDrawCalls 和
instancedObjects（按 Pass/cascade 提交计数，不是去重 Entity）。关闭时不收集实例/编译变体。
D3D11/D3D12 隐藏窗口 readback 各通过 1/1 用例、20/20 断言：6 个非均匀/shear/镜像
对象的 GBuffer 与 Shadow 均由 6 draws 降为 2，法线差异 0、阴影最大误差 0。
这证明该夹具的图像一致与提交减少，不能推导所有场景的帧率提升。用户编辑器画面仍待验收。
补充门禁：旧 TAA fixed-grid GPU 用例有 9 个断言失败，隔离旧 Shader 编译器仍得到相同
结果；生产 TAAPass 未被本轮修改。后续专项已定位为 AYShader 的 else-if/运算符/语法
错误门禁缺陷并修复；未修改 TAA 算法或容差。D3D11/12 各 10 cases/6339 assertions
通过，含新 debug mode 回归，详见 testing.md 的 2026-10-03 TAA fixed-grid 修复记录。

### R6-6：诊断与真 GPU 门禁（已完成）

- 调试视图：MotionVector、SSAO、TAA history/resolve、Shadow atlas。
- 每 Pass CPU/GPU 时间、transient RT 数量/峰值和 graph compile 摘要。
- D3D11 capture 覆盖 MRT、阴影、Bloom、Haze、SSAO、TAA、透明和 resize。
- D3D12 本阶段最低要求是启动/初始化错误可报告且不闪退；完整画面对齐可独立排期。

第一刀完成项（2026-09-21）：

- 保留既有每 Pass CPU/GPU 时间，在公开 `RenderFrameStats` 尾部追加 backend-neutral 的 FrameGraph 与资源黑板摘要。
- FrameGraph 摘要覆盖 compile 成败/错误数、declared/live pass、logical resource、当前/峰值 transient target 和 retention 状态；资源黑板摘要覆盖 declared/available/valid/produced/invalid/history 数量。
- bgfx debug text overlay 可直接显示 `FG` 与 `BB` 两行；overlay 关闭时宿主仍可通过 `getFrameStats()` 读取，不增加 GPU 工作。
- 定向统计回归与 Renderer 集成验证通过；全量 `AYRenderer_Test` 为 4677/4677，`AYEditorShell_Demo` 链接通过。

第二刀完成项（2026-09-21）：

- 复用 view 250 的纹理覆盖层并保持原 GBuffer 0–5 通道数值，追加 6 MotionVectors、7 SSAO Occlusion、8 TAA History、9 Shadow Atlas；宿主 byte channel API 保持兼容。
- 四个新通道共享一个按选择绑定的 auxiliary sampler；默认关闭时零 draw、零 RT，不插入新 Pass，也不改变主渲染顺序。
- MotionVector/SSAO 只显示资源黑板的当帧已产出内容，TAA History 只显示有效 persistent read history，Shadow Atlas 只显示 ShadowPass 明确发布的可采样深度；缺失时 fail-close，避免把 stale texture 当成诊断结果。
- 保留既有 view 245 的 TAA Final、History rejection、History weight、Clipping difference、Motion 与 Reprojected history 模式，原始 history 通道用于补齐资源级观察，不复制 resolve 诊断。
- Phoskia IR 与 Windows D3D11 `s_5_0` 生产编译定向回归通过；全量 `AYRenderer_Test` 为 4694/4694，`AYEditorShell_Demo` 链接通过。Noop 空后处理图的诊断预期同步修正为 declared/live 0/0，不把 CPU-only 管线节点伪装成 FrameGraph 节点。

D3D11 基线 capture（2026-09-21）：

- RenderDoc 1.46 在 1536×912 窗口、1083×594 视口和 4×MSAA 下连续捕获三个正常帧；主链覆盖 Shadow、Skybox、GBuffer、MotionVector、SSAO、Lighting、DepthHaze、Transparent、Bloom、PostProcess、TAA、Present 与 Editor/UI overlay。
- 抓帧确认 MRT/Depth、Motion、SSAO、HDR scene、Bloom、FinalLdr 与 TAA history 的尺寸和格式符合契约；生产者先写、消费者后读，没有发现主链倒序。
- TAA history 在 frame 24490/24663/24686 依次执行 A←B、B←A、A←B，且 Present 读取同帧 resolve 输出；静态 history ping-pong 门禁已关闭。
- 基线证据见 [`d3d11-capture-report-2026-09-21.md`](d3d11-capture-report-2026-09-21.md)。

最终动态/失效 capture（2026-09-23）：

- 24 个独立 D3D11 capture 补齐 selection、resize 首帧/稳定帧、camera cut、效果关闭/重开、透明边界、10 个 raw resource channel、5 个 TAA diagnostic mode 与双灯 Shadow atlas。关闭帧不含被禁用效果的 view，重开首帧恢复有效目标；resize 首帧未混用资源代际。
- Selection Outline 改为 view 244 未 jitter Alpha silhouette + view 253 jittered/depth-tested RGB visibility + view 254 稳定栅格合成，不再以反向偏移尝试抵消 coverage 变化。
- D3D12 编辑器启动烟测保持运行 15 秒；初始化失败返回路径会持久记录、显示错误并安全退出。完整画面对齐不属于本阶段最低门禁。
- 最终证据见 [`d3d11-capture-report-2026-09-23.md`](d3d11-capture-report-2026-09-23.md)。`TAA-TRANSPARENCY-01` 继续作为明确记录的透明 depth/motion 能力限制，不影响 R6-6 基础门禁关闭。

## 4. 新画质能力的恢复顺序

R6-2、R6-3 和 D3D11 基线完成后再恢复新增效果：

1. MotionVector/SSAO/TAA/Shadow 调试节点；
2. TAA velocity dilation 与透明 velocity/reactive mask；（2026-09-23 已完成基础版；多层透明/折射仍后续）
3. Shadow 质量：CSM、Spot 透视锥体（2026-09-23 基础版已完成，待真 GPU 验收），再评估 Point omni；
4. Motion Blur；
5. Bloom 金字塔与 Auto Exposure；
6. DOF；
7. Hi-Z 后的 GTAO/SSR；
8. 体积雾等高成本效果。

Motion Blur 不直接消费当前基础 velocity 后就宣称完成：必须先锁定 camera cut、遮挡边界、速度膨胀和透明/reactive-mask 契约。

## 5. 每刀守门

- 不改变公开枚举既有数值和公开结构已有字段顺序。
- 修改共享私有布局后执行完整依赖重建，防止 stale object/ABI 假故障。
- shader 修改必须更新 cache key，并跑 Windows `s_5_0` 生产编译。
- FrameGraph compile 失败必须 fail-close，不允许继续创建未验证的 owned target。
- 每刀至少通过定向测试、`AYRenderer_Test` 全量和 `AYEditorShell_Demo` 链接。
