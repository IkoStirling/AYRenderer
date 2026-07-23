# AYRenderer Pass 路线（2026-07-23 更新）

> 与 [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) / [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) / [`shadow-pass.md`](shadow-pass.md) / [`deferred-pass.md`](deferred-pass.md) 配套。  
> 记录 Shadow 验收后 + Deferred 闭环后的优先级，避免按过期 `execution-plan.md` 开工。

## 当前执行入口（必读）

| 阶段 | 文档 | 状态 |
|------|------|------|
| **短期（正在做）** | [`short-term-plan.md`](short-term-plan.md) | **唯一开工清单** — 下一刀 Bloom MVP |
| **中期（只读 cutsheet）** | [`frame-graph-mvp.md`](frame-graph-mvp.md) | 未开工；PP 链 ≥2 效果后再实现 |
| **Deferred 验收锁** | [`deferred-acceptance.md`](deferred-acceptance.md) | 已钉；回归先查此表 |

**不要**在短期直接实现 FrameGraph / SSAO / TAA / CSM。

## 管线归属（答「谁组合的」）

- **Pass 列表与执行**在 **AYRenderer**：`RenderPipelineDesc::makeDefault()` =  
  `Shadow → ForwardOpaque → Transparent → PostProcess → UI`（`makeForwardWithShadows()` 同义别名）。
- **AYEditor 不自组 Pass**，只调用 `renderer.configurePipeline(...)` 选用管线，并设 PP / 灯光 / 天空 knobs。
- **Deferred opt-in**：`configurePipeline(makeDeferred())` =  
  `Shadow → Skybox → GBuffer → Lighting → Transparent → PostProcess → UI`。**默认 Forward 不变**（§5.3 红线 #4）。
- 换槽位 / 关 Shadow / 切 path：宿主传自定义 `RenderPipelineDesc`。

## 当前默认 / Deferred 管线

```
Forward:  Shadow → ForwardOpaque → Transparent → PostProcess → UI
Deferred: Shadow → Skybox → GBuffer → Lighting → Transparent → PostProcess → UI
```

| Pass | 状态 | 说明 |
|------|------|------|
| **ForwardOpaque** | 可用 | 含阴影采样；Alpha skip 给 Transparent |
| **Shadow** | 表现通过 | key-only + sampleReady；atlas 槽位日志 `atlasSlots`≠网格绘制数 (§S2-3,2026-07-23 重命名) |
| **Skybox** | Deferred 可用 | equirect + 独立 IBL cube |
| **GBuffer** | Deferred 可用 | RT2 = worldPos RGBA16F（`gbuffer_v7`） |
| **Lighting** | Deferred 可用 | `lighting_v26`；点/聚光 + IBL |
| **Transparent** | 可用 | 距离 sortKey |
| **PostProcess** | Final 可用 | ACES + gamma；假 bloom 待 S1 替换 |
| **UI** | 可用 | Game View 合成 |

## 优先级（2026-07-23）

1. ~~Deferred 最小闭环~~ — 已绿（见 acceptance）。  
2. **短期：Bloom MVP 手写链** — [`short-term-plan.md`](short-term-plan.md) 刀 S1。  
3. **中期：轻量 FrameGraph（仅 PP 段）** — [`frame-graph-mvp.md`](frame-graph-mvp.md)。  
4. **Shadow 增强（可选）** — CSM / PCSS（与 FG 正交）。  
5. SSAO / SSR / TAA — FG 落地后再挂。

## 纪律（抄自 Shadow lessons）

- bgfx 目标：灯光/标量类 uniform 用 **vec4 + `.x`**，禁止依赖 GLSL uniform 初值。  
- 先 hand / emit 对齐，再切默认；改 shader 必 bump cache key。  
- 默认管线变更严守 §5.3 ── 新 Pass 必须 opt-in，不变默认。  
- FrameContext / RenderScene 多光源数据**永不**进；走 `PassExecContext::*` 借用指针。  
- Pass 内禁止公开头直 include `<bgfx/bgfx.h>`（P6.5）。
