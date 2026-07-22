# AYRenderer Pass 路线（2026-07-22）

> 与 [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md)、[`shadow-pass.md`](shadow-pass.md) 配套。  
> 记录 Shadow 验收后的优先级，避免再按过期的 `execution-plan.md`（2026-07-20）开工。

## 当前默认管线

```
Shadow → ForwardOpaque → Transparent → PostProcess → UI
```

| Pass | 状态（Editor） | 说明 |
|------|----------------|------|
| **ForwardOpaque** | 可用 | 含阴影采样；Alpha 已 skip 给 Transparent |
| **Shadow** | 表现基本通过 | R8 深度 + PCF；Phoskia 默认，`.sc` 可 A/B |
| **Transparent** | 槽位可用、半成品 | 画 Alpha；`sortKey` 需宿主填相机距离 |
| **PostProcess** | **已做实（波纹）** | 见 [`post-process.md`](post-process.md)；Editor 默认开小强度 |
| **UI** | 可用 | Game View 合成 |
| **GBuffer / Lighting** | 未做 | 延迟整条，排在前向链路之后 |

## 优先级

1. ~~**PostProcess 做实**~~ — 波纹滤镜已落地，见 [`post-process.md`](post-process.md)。  
2. **Transparent 补齐** — `RenderSystem` 写 `sortKey`；放 Alpha 物体验收。  
3. **Shadow 增强（可选）** — CSM / pack 深度等。  
4. **GBuffer + Lighting** — 大工程，前向稳后再开。

## 纪律（抄自 Shadow lessons）

- bgfx 目标：灯光/标量类 uniform 用 **vec4 + `.x`**，禁止依赖 GLSL uniform 初值。  
- 先 hand / emit 对齐，再切默认；改 shader 必 bump cache key。
