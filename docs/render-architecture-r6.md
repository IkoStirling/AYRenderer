# AYRenderer R6 架构收口计划

> 日期：2026-09-09
> 状态：R6-0、R6-1、R6-2、R6-3、R6-4 已落地；R6-5 第一阶段已落地；R6-6 第一、二刀已落地，真 GPU 门禁仍开放。
> 范围：在不重写 bgfx/RHI、不改变 Forward/Deferred 画面顺序的前提下，收口 Pass 描述、资源依赖、诊断和场景提交。

## 1. 当前真实基线

当前功能链已经覆盖 Shadow、GBuffer、MotionVector、SSAO、Lighting、DepthHaze、Transparent、Camera Overlay 2D、Bloom、PostProcess、TAA/FXAA/SMAA、ColorGrading、Present、Editor Overlay 与 UI。继续直接增加效果会放大以下结构问题：

- `RenderPipeline` 仍按注册顺序执行 `RenderPass`，但带 slot 的 Pass 已由编译后的 `FrameGraph` live decision 控制是否 dispatch；资源黑板已覆盖时域历史、Deferred 主链与 Bloom 链的可消费输出。
- `Renderer::render()` 同时负责 Pass 查找、状态预备、效果门禁、资源声明、semantic 选择和执行上下文拼装。
- `PassExecContext` 通过多个具体 Pass 指针传递生产者状态，新增效果容易继续增加横向耦合。
- 几何 Pass 已复用每帧一次生成的共享 DrawList 做领域分桶与稳定排序；保守起见尚未启用 frustum cull、状态排序合批与 instancing。
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

### R6-5：DrawListBuilder（第一阶段已完成）

- 每帧一次完成 opaque/cutout/transparent/shadow/overlay 分桶和稳定排序。
- 第一阶段已复用过滤结果，仍逐对象 submit；GBuffer、MotionVector、ForwardOpaque、Forward2DOpaque、Transparent 与 Shadow 共用同一份分类结果。
- frustum cull 暂缓：必须先统一静态、蒙皮、透明描边和阴影接收体的可靠 bounds 策略，避免以 CPU 优化引入物体错误消失。
- 第二阶段再对相同 mesh/material/state 的非蒙皮对象启用 bgfx instancing。

验收：画面与 draw 顺序不变；CPU pass 时间下降；透明排序和 MotionVector stable id 不回归。

### R6-6：诊断与真 GPU 门禁（进行中）

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

下一步：按 [`d3d11-capture-checklist.md`](d3d11-capture-checklist.md) 执行固定场景 capture，并记录 MRT、Shadow、Bloom、Haze、SSAO、TAA、透明与 resize 的资源/时序证据。本机当前未安装 RenderDoc，因此 R6-6 真 GPU 门禁仍保持开放，不以 shader 编译、Noop 单测或普通截图代替。

## 4. 新画质能力的恢复顺序

R6-2、R6-3 和 D3D11 基线完成后再恢复新增效果：

1. MotionVector/SSAO/TAA/Shadow 调试节点；
2. TAA velocity dilation 与 reactive mask；
3. Shadow 质量：CSM、Spot 透视锥体，再评估 Point omni；
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
