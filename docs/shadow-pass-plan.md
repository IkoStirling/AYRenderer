# ShadowPass 工业级重写计划

> 对齐 bgfx `examples/16-shadowmaps`，保留 AY 多 Pass + Phoskia + PassExecContext 架构。  
> 当前 Editor 验收场景：cube @(0, 0.85, 0) + ground @(0, -0.05, 0) scale 8，方向光 (0.35, -0.85, -0.40)。  
> **状态：Phase 0–6 全部完成**（stamp `v12-phase6-resolve-view`）。使用说明见 [`shadow-pass.md`](shadow-pass.md)。

## 一、现状与根因（历史）

| 约定 | bgfx example 16 | 旧 AY 实现 | 后果 |
|------|-----------------|------------|------|
| Caster 矩阵 | `setViewTransform` + `modelViewProjection` | 额外 `u_lightViewProj` + clip.xy hack | 裁剪/写入失败 |
| 深度语义 | `z/w * 0.5 + 0.5` + pack/unpack | 手写 `(z/w-0.1)/23.9` | compare 与 GPU 不一致 |
| Shadow RT | BGRA8 + D32F，正常 depth test | RGBA8 + DEPTH_TEST_ALWAYS | 深度 buffer 不参与 |
| 物体过滤 | 全部 caster | 按 scale 跳过 ground | 不可扩展 |

日志 `atlasSlots=1 blitOk=1` 全绿但 map 仍为 clear 1.0 → **caster 未 rasterize**，不是 compare 小错。 (§S2-3 — 字段名重命名,语义不变)

## 二、目标架构

```
Host: RenderPipeline [Shadow → FO → Trans → PP → UI]
      PassExecContext.shadowPass (borrowed pointer)

Shadow Subsystem:
  ShadowSettings        — near/far/bias/size/build stamp
  ShadowDepthCodec      — ndc01 + pack/unpack + compare (CPU/GPU 一致)
  ShadowShaderSources   — caster + receiver phoskia
  ShadowMapResources    — FBO + resolve + lifecycle (Phase 2)
  ShadowMatrixBuilder   — scene AABB fit (Phase 3)
  ShadowCaster          — program + draw loop (Phase 4)
  ShadowReceiverContract— material bind contract (Phase 5)
  ShadowDiagnostics     — env toggles + L0–L4 + frame stats (Phase 6)

Consumers: ForwardOpaquePass / TransparentPass via tryBindShadowSampler()
```

**设计原则**

1. 单一矩阵源：`setViewTransform(shadowView, lightView, lightProj)`；receiver 上传同一 LVP 字节。
2. 单一深度语义：`ShadowDepthCodec` 与 shader 共用 `ndc01 = z/w*0.5+0.5`。
3. 显式 caster 标记：`DrawItem::shadowFlags`，不用 scale 启发式。
4. Producer/Consumer 契约：`shadowSampleTexture()` + `lightViewProjColumnMajor()` + `isReady()`。
5. GPU readback 仅 1×1 probe，不对 FO 采样的 resolve 纹理 READ_BACK。

## 三、分阶段计划（全部完成）

| Phase | Stamp | 内容 |
|-------|-------|------|
| 0 | — | 基线 / 环境变量 |
| 1 | `v12-phase1-bgfx-ndc01` | Settings / Codec / ShaderSources |
| 2 | `v12-phase2-shadow-resources` | FBO + LESS depth |
| 3 | `v12-phase3-scene-fit-ortho` | ShadowMatrixBuilder |
| 4 | `v12-phase4-shadow-flags` | ShadowCaster + shadowFlags |
| 5 | `v12-phase5-receiver-contract` | Receiver contract + bind |
| **6** | **`v12-phase6-resolve-view`** | docs + L0–L4 + **LVP = P×V 对齐 setViewTransform** |

## 四、扩展预留（未做，不算本次迁移范围）

| 未来 | 预留 |
|------|------|
| CSM | `ShadowSettings::numCascades` + FBO array |
| PCF/PCSS | receiver shader variant |
| Point/Spot | `ShadowMatrixBuilder` 按 light type 分派 |
| 默认 pipeline Shadow | `RenderPipelineDesc::enableShadows`（E4 实验通过后） |

## 五、Definition of Done（Editor）

1. `build=v12-phase6-resolve-view`
2. `groundUnderCube`: `refN≈0.51`, `uv≈(0.50, 0.63)`（`AY_SHADOW_LOG=3`）
3. `AY_SHADOW_DEBUG=1`：cube 区域灰度 ≈0.51，非全白
4. 正常 Play：contact shadow 可见
5. `AY_SHADOW_CASTER_SOLID=1`：map 全 ~0.5
6. `AYRenderer_Test` shadow cases 全绿

## 六、文档

- 使用说明：[`docs/shadow-pass.md`](shadow-pass.md)
