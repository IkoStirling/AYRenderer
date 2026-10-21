# AYRenderer Pass 路线（2026-10-22 更新）

> 与 [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) / [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) / [`shadow-pass.md`](shadow-pass.md) / [`deferred-pass.md`](deferred-pass.md) 配套。  
> 记录当前 Pass 基线与执行入口。2026-07 的 Bloom/FrameGraph cutsheet 已完成并转为历史记录，不再作为开工顺序。

## 当前执行入口（必读）

| 阶段 | 文档 | 状态 |
|------|------|------|
| **当前结构** | [architecture.md](architecture.md) | 实际源码地图、产品链、资源/线程边界 |
| **后续方向** | [根渲染路线](../../../AYDocs/AYRendering-Architecture-Roadmap.md) | 已实现状态与后续提案分别记录 |
| **R6（基础收口完成）** | [`render-architecture-r6.md`](render-architecture-r6.md) | R6-1/R6-2/R6-3/R6-4、R6-5 第一阶段与 R6-6 均已完成 |
| **历史 cutsheet** | [`short-term-plan.md`](short-term-plan.md)、[`frame-graph-mvp.md`](frame-graph-mvp.md) | Bloom/Haze/SSAO/FrameGraph MVP 已落地，仅供追溯 |
| **Deferred 验收锁** | [`deferred-acceptance.md`](deferred-acceptance.md) | 已钉；回归先查此表 |

R6-4 与 D3D11 capture 基线已完成；新增画质 Pass 可按后续路线恢复，但仍须遵守本页门禁。

## 管线归属（答「谁组合的」）

- **组合策略**在 `RenderPipelineDesc.cpp`，实例工厂在 `RenderPipelineFactory.cpp`；`RenderPipeline` 拥有实例并按 descriptor 顺序 dispatch。FrameGraph/黑板管理逻辑资源与当前内容；设备顺序另由 View remap 明确排序。
- **AYEditor 不自组 Pass**，只调用 `renderer.configurePipeline(...)` 选用管线，并设 PP / 灯光 / 天空 knobs。
- **Deferred opt-in**：Editor/宿主调用 `configurePipeline(makeDeferred())`；Forward 仍是 Renderer 产品默认。
- 换槽位 / 关 Shadow / 切 path：宿主传自定义 `RenderPipelineDesc`。

## 当前默认 / Deferred 管线

```
Forward:  Shadow → ForwardOpaque → DepthHaze(no-op) → Transparent
          → CameraOverlay2D → Bloom → AutoExposure → PostProcess → FXAA/SMAA
          → StylizedOutline → ColorGrading → ColorStylization
          → TemporalFeedback → AnalogDisplay → Present → UI

Deferred: Shadow → Skybox → GBuffer → DeferredDecal → MotionVector(on demand) → SSAO
          → Lighting → SSR → DepthHaze → Volumetric → Transparent → CameraOverlay2D
          → Bloom → AutoExposure → PostProcess → TAA/FXAA/SMAA
          → StylizedOutline → ColorGrading → ColorStylization
          → TemporalFeedback → AnalogDisplay → Present → UI
          → optional GBufferDebug
```

| Pass | 状态 | 说明 |
|------|------|------|
| **ForwardOpaque** | 可用 | 含阴影采样；Alpha skip 给 Transparent |
| **Shadow** | 可用；具体设备证据见testing | 八槽 atlas；方向光CSM、Spot透视锥体；Point omni仍为后续项 |
| **Skybox** | Deferred 可用 | equirect + 独立 IBL cube |
| **GBuffer / Lighting** | Deferred 可用 | 四MRT与HDR Lighting；当前验收和限制见模块设计/测试，旧未关门禁不再作为实时状态 |
| **MotionVector / TAA** | Deferred 可用 | opaque/cutout/transparent 刚体与蒙皮 velocity、3×3 有效速度 dilation、透明 reactive feedback + persistent history |
| **SSAO / Haze / Bloom** | 可用 | FrameGraph logical resource + current-frame production latch |
| **SSR / Volumetric / DeferredDecal** | 显式可选 | 高级Host路径已实现；不等于Hi-Z、全部透明语义或全场景规模已验收 |
| **AutoExposure / Color / Temporal / Display** | 显式可选 | 各自设置与失效边界，按主视口执行；独立预览不继承整链 |
| **Transparent / CameraOverlay2D** | 可用 | 3D 透明稳定排序；2D 使用独立正交相机并在 3D 后合成 |
| **PostProcess / AA / ColorGrading / Present** | 可用 | 唯一 backbuffer 边界；TAA/FXAA/SMAA 互斥 |
| **UI / EditorOverlay / GBufferDebug** | 可用 | Present 后的编辑器与 UI 覆盖层 |

## 已完成结构与后续优先级

1. **R6-1 FrameGraph 编译契约（已完成）** — 结构校验、semantic roots、终端反向裁剪已落地。
2. **R6-2 图计划外移（已完成）** — 后处理图声明已提取为不持有具体 Pass 的 `PostProcessGraphPlan`。
3. **R6-4 资源黑板与历史资源（已完成）** — MotionVector/TAA history、GBuffer/SSAO/DepthHaze、Lighting/Skybox/Bloom 的可消费纹理输出已集中登记；具体 Pass 指针仅保留非纹理元数据和单测兼容用途。
4. **R6-5 FrameDrawLists与刚体实例化** — 已有共享分类、可靠静态剔除与连续兼容GBuffer/Shadow实例化；更广的变形bounds/材质规模化仍需独立验收。
5. **R6-6 诊断与 D3D11 capture（已完成）** — graph/blackboard/RT 统计、raw resource/TAA 调试入口与真实 GPU 动态/失效抓帧均已完成；证据见 2026-09-23 capture report。

下一步先处理GPU刷新结果确认、驻留观察/预算和资源所有权，再推进语义mip、绑定采样与结构收口。
新效果按独立需求立项，完整细目以根渲染路线与2026-10-22源码自审为准。

## 纪律（抄自 Shadow lessons）

- bgfx 目标：灯光/标量类 uniform 用 **vec4 + `.x`**，禁止依赖 GLSL uniform 初值。  
- 先 hand / emit 对齐，再切默认；改 shader 必 bump cache key。  
- 新效果默认关闭，默认管线行为变化需明确契约及设备证据。
- SceneLights可由RenderScene持有值快照；PassExecContext只在同步dispatch借用选定来源。FrameContext不增加可变灯光容器。
- Pass 内禁止公开头直 include `<bgfx/bgfx.h>`（P6.5）。
