# AYRenderer Pass 路线（2026-09-09 更新）

> 与 [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) / [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) / [`shadow-pass.md`](shadow-pass.md) / [`deferred-pass.md`](deferred-pass.md) 配套。  
> 记录当前 Pass 基线与执行入口。2026-07 的 Bloom/FrameGraph cutsheet 已完成并转为历史记录，不再作为开工顺序。

## 当前执行入口（必读）

| 阶段 | 文档 | 状态 |
|------|------|------|
| **R6（正在做）** | [`render-architecture-r6.md`](render-architecture-r6.md) | **唯一开工清单** — R6-1/R6-2 已完成，下一刀 R6-3 |
| **历史 cutsheet** | [`short-term-plan.md`](short-term-plan.md)、[`frame-graph-mvp.md`](frame-graph-mvp.md) | Bloom/Haze/SSAO/FrameGraph MVP 已落地，仅供追溯 |
| **Deferred 验收锁** | [`deferred-acceptance.md`](deferred-acceptance.md) | 已钉；回归先查此表 |

R6-2/R6-3 与 D3D11 基线完成前，默认不新增独立画质 Pass；现有 Pass 修复、诊断节点与架构收口除外。

## 管线归属（答「谁组合的」）

- **Pass 列表与执行**在 **AYRenderer**：`RenderPipeline` 仍按 descriptor 注册顺序执行；FrameGraph 管理后处理 logical resource、semantic、本帧生产状态与 transient target。
- **AYEditor 不自组 Pass**，只调用 `renderer.configurePipeline(...)` 选用管线，并设 PP / 灯光 / 天空 knobs。
- **Deferred opt-in**：Editor/宿主调用 `configurePipeline(makeDeferred())`；Forward 仍是 Renderer 产品默认。
- 换槽位 / 关 Shadow / 切 path：宿主传自定义 `RenderPipelineDesc`。

## 当前默认 / Deferred 管线

```
Forward:  Shadow → ForwardOpaque → DepthHaze(no-op) → Transparent
          → CameraOverlay2D → Bloom → PostProcess → FXAA/SMAA
          → ColorGrading → Present → UI

Deferred: Shadow → Skybox → GBuffer → MotionVector(on demand) → SSAO
          → Lighting → DepthHaze → Transparent → CameraOverlay2D → Bloom
          → PostProcess → TAA/FXAA/SMAA → ColorGrading → Present → UI
          → optional GBufferDebug
```

| Pass | 状态 | 说明 |
|------|------|------|
| **ForwardOpaque** | 可用 | 含阴影采样；Alpha skip 给 Transparent |
| **Shadow** | 可用，真 GPU 门禁未关 | 八灯 atlas；Point omni 与 Spot 透视锥体仍是能力项 |
| **Skybox** | Deferred 可用 | equirect + 独立 IBL cube |
| **GBuffer / Lighting** | Deferred 可用 | 四 MRT + HDR Lighting；真实 capture/带宽门禁未关 |
| **MotionVector / TAA** | Deferred 可用 | opaque 刚体/蒙皮 velocity + persistent history；透明 reactive mask 未做 |
| **SSAO / Haze / Bloom** | 可用 | FrameGraph logical resource + current-frame production latch |
| **Transparent / CameraOverlay2D** | 可用 | 3D 透明稳定排序；2D 使用独立正交相机并在 3D 后合成 |
| **PostProcess / AA / ColorGrading / Present** | 可用 | 唯一 backbuffer 边界；TAA/FXAA/SMAA 互斥 |
| **UI / EditorOverlay / GBufferDebug** | 可用 | Present 后的编辑器与 UI 覆盖层 |

## 优先级（2026-09-09）

1. **R6-1 FrameGraph 编译契约（已完成）** — 结构校验、semantic roots、终端反向裁剪已落地。
2. **R6-2 图计划外移（已完成）** — 后处理图声明已提取为不持有具体 Pass 的 `PostProcessGraphPlan`。
3. **R6-3/R6-4 Pass 契约与资源黑板（当前）** — 减少字符串查找、具体 Pass 指针与 stale handle 风险。
4. **R6-5 DrawListBuilder** — 共享可见集、分桶，再做 instancing。
5. **R6-6 诊断与 D3D11 capture** — 完成后再恢复新画质 Pass。

## 纪律（抄自 Shadow lessons）

- bgfx 目标：灯光/标量类 uniform 用 **vec4 + `.x`**，禁止依赖 GLSL uniform 初值。  
- 先 hand / emit 对齐，再切默认；改 shader 必 bump cache key。  
- 默认管线变更严守 §5.3 ── 新 Pass 必须 opt-in，不变默认。  
- FrameContext / RenderScene 多光源数据**永不**进；走 `PassExecContext::*` 借用指针。  
- Pass 内禁止公开头直 include `<bgfx/bgfx.h>`（P6.5）。
