# Deferred Pass 工业级重写计划

> 对齐工业级多光 deferred shading（GBuffer MRT + Lighting pass + 全屏三角形）。  
> 保留 AY 多 Pass + Phoskia + PassExecContext 架构，**复用 Shadow 集成的 7 文件 + 双 viewId 模板**。  
> **状态：B0 docs ship（2026-07-22）── 现场重置, 0 代码**。  
> 配套使用说明：[`deferred-pass.md`](deferred-pass.md)；教训：[`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md)；项目级别：[`execution-plan.md`](execution-plan.md) §P5 / §5.3。

## 一、现状与根因（历史）

### A. Forward path 瓶颈

| 项 | 现状 | 后果 |
|----|------|------|
| 每盏方向光 = 一次 FO loop | ForwardOpaquePass 不分光遍历 draw item list | n 盏光 × N draw call = O(n×N) per-frame |
| 半透明 | 单独 TransparentPass,但走 GPU blend + 排序仍 O(N log N) | 中规模场景 OK,大场景耗时 |
| Light struct 支持 | **无** ── 只 1 盏方向光写 `FrameContext::lightDirection` | 多光内容创作受限 |

### B. Shadow 集成成本作为 deferred 集成预算基线

集成 Shadow 实际开销（[[shadow-pass-plan.md](shadow-pass-plan.md)] + 后续 P3/F2/F3 + E4 + E5）：
- 7 文件 `Shadow{Caster,MatrixBuilder,MapResources,LightMatrix,Debug,ReceiverContract,Pass}` + `AYShadow*` 配置头
- 2 viewId（caster + resolve blit）
- blit→resolve 1×1 probe 链 + diff-with-emit 反复 bisect
- 4-cut bisect（PR-F1 / F1' / F2 / F3 / E4 / E5）— 含一次 PR-F1 **全 SIGSEGV**（§5.5 历史）
- Default pipeline 翻转 + cleanup + 边界 trap 修复（B2 等）

### C. Deferred 触碰面预测（vs shadow）

| 维度 | Shadow | Deferred 预测 |
|---|---|---|
| 文件数 | 7 | 8–10（多 GBuffer receiver contract + Lighting receiver contract + Lighting shader 源）|
| **ViewId** | 2（caster + resolve） | **4**（caster + GBuffer MRT = view 7 + Lighting = view 8 + shadow caster 复用 view 1）|
| **MRT / blit** | **单 depth FBO** | **4-slot MRT（GBuffer 5 attachment：RT0–2 RGBA8 + RT3 D24S8）+ 单独输出（Lighting）**|
| 多光源 / struct | 无（1 盏走 FrameContext） | **1 盏 → 1 盏 DataSource → 多盏 DataSource ── 多刀** |
| Phoskia multi-output | 无（单 frag out） | **必须** support multi-frag-out 或先 hand `.sc` |
| §5.3 红线面 | 4 | 全 4，重合 |
| 大 bang 风险 | **极高** ── PR-F1 SIGSEGV 历史 | **极高** ── 镜像翻车点 |

> ⚠ View id 现状（2026-07-22 E5 ship baseline）：**0–6 全部占用** ── 0=clear / 1=ShadowC / 2=ShadowR / 3=FO / 4=Trans / 5=PP / 6=UI。Deferred 必须新加 **view 7 / view 8** ── **不复用 1–6 任何**。

> ⚠ GBuffer depth 走 **硬件 D24S8**（standard bgfx 模式），**不复刻 Shadow R8 workaround**（ShadowMapResources 专用，详见 lessons §3.6）。

> ⚠ 现网 `BGFXAdapter::createFrameBuffer` 只能 1× color 或 1× color + 1× depth。B4 **必须**新加 `createGbufferFrameBuffer` API 给 5-attach MRT 用。

→ Deferred 落地预算 = **比 Shadow 多 1.5× 文件 + 2× 视图 + 一次 multi-output 编译调试**，每刀 ≤ 8 文件硬约束 + §5.4 隔离实验规矩。

## 二、目标架构（B6 ship 后形态）

```
Host: RenderPipeline [Default Forward | Deferred (opt-in)]
      PassExecContext.shadowPass (borrowed)
      PassExecContext.gbufferPass (borrowed, B2)

Deferred Subsystem:
  GBufferSettings        — mrt size / format / build stamp
  GBufferShaderSources   — caster phoskia multi-output
  GBufferMapResources    — RGBA8×3 + D24S8 FBO + lifecycle
  GBufferCaster          — draw loop + viewport / FBO bind
  GBufferReceiverContract — material MRT contract

  LightingSettings       — build stamp + shader variant
  LightingShaderSources  — fullscreen tri + GBuffer sample + Shadow reuse
  LightingResources      — fullscreen VB + LightingOutput FBO
  LightingPass           — sample GBuffer + sum per-light + write back

  PassDiagnostics        — L0–L4 + frame stats

Consumers: Transparent (alpha only) → PostProcess (scene-fbo fallback chain)
           UI (unchanged)
```

**设计原则**（继承 shadow lessons）：

1. **单一矩阵源** ── LightingPass 复用 `FrameContext::lightDirection`（1 盏）；多光走 `ctx.lights` 借用指针（B7+），**不**进 FrameContext 绝不进 RenderScene。
2. **GBuffer 私有 / Lighting 只读 + host 共享 PostProcess** ── Mirror `shadowSampleTexture()` getter 模式：`gbufferPass()->albedoTexture() / normalTexture() / motionTexture() / depthTexture() / lightingOutputFbo()`。
3. **显式 path 选择** ── `RenderPath` enum + `RenderPipelineDesc::makeDeferred()` opt-in；默认 Forward 不变（§5.3 #4）。
4. **深度语义**：GBuffer RT3 = 硬件 D24S8，**不复刻 Shadow R8 workaround**（ShadowMapResources 专用，详 lessons §3.6 + shadow-pass.md L113）。LightingPass fragment 不用 pack/unpack，直接 sample D24S8 linearize helper（或与 viewProj 反推 depth 比对 — Phoskia converter 自带）。
5. **诊断 L0–L4** ── `AY_DEFERRED_LOG=0..4`；stamp 写入 stderr（`build=…`）。

## 三、分阶段计划

| B | Stamp | 内容 | 触碰面 |
|---|-------|------|--------|
| **B0** ✅ | `v13-phase7-vec4-abi` (docs only) | 现场重置：deferred-pass.md + pass-lessons-from-deferred.md + execution-plan.md §P5 + roadmap | `docs/*` |
| B1 | `v14-phase8-renderpath-enum` | `enum RenderPath` + `RenderPipelineDesc::path` 默认 Forward | `AYRenderTypes.h` / `RenderPipeline.{h,cpp}` / tests (2–3) |
| B2 | `v14-phase8-gbuffer-stub` | `GBufferPass` 空壳 + `PassExecContext::gbufferPass` 借用指针 + Noop 0-draw | `GBufferPass.{h,cpp}` / `PassExecContext.h` / tests (5–7) |
| B3 | `v14-phase8-lighting-stub` | `LightingPass` 空壳 + Forward/Deferred path 显式切换 | `LightingPass.{h,cpp}` / `RenderPipeline.*` / tests (4–6) |
| **B4** | `v14-phase8-gbuffer-mrt` | GBuffer 真 MRT：RT0 albedo / RT1 normal / RT2 motion / RT3 depth | `GBufferResources.{h,cpp}` / `GBufferPass.cpp` / Phoskia 源 / tests + docs (6–8) |
| **B5** | `v14-phase8-lighting-realtime` | LightingPass 真光 + 1 盏方向光 parity vs Forward | `LightingPass.cpp` / Phoskia 源 / tests + docs (6–8) |
| **B6** | `v14-phase8-deferred-default-off` | `makeDeferred()` factory + 默认 Forward 不变 + PostProcess source-FBO 选择优先级 + docs 收口 + 附录 A | `AYRenderTypes.h` / `RenderPipeline.*` / `PostProcessPass.cpp` / docs (3–5) |

每 B ≤ 8 文件 + 3 跑稳通过 → 进下一 B。

## 四、扩展预留（B7+ / Round 2, 不在 B0–B6）

| 未来 | 预留 |
|------|------|
| 多光源 DataSource | `PassExecContext::lights = const LightsArray*` 借用指针（**严禁 RenderScene::Light struct**）|
| 点光 / 聚光 | LightingPass 按 light type 分派；GBuffer 不变 |
| 环境光照 / IBL | LightingPass 累加 env lobe；GBuffer 加 normal 格式视情 |
| 皮肤 / 半透明 Forward sub-pass 在 Deferred path | Pipeline 用 desc 选 FO/Trans sub-slots |
| GBuffer alias（spec / sssMask / velocity） | v1 锁 4 slot；alias 走 multi-render-target-slots |
| Deferred MSAA resolve | 主 RT 已是 viewport 大小；不必 |
| CSM 复用 | LightingPass 跟 ShadowPass 同样存 slot count（与 Shadow 增强同步） |

## 五、Definition of Done（B6 ── Editor）

1. `build=v14-phase8-deferred-default-off`
2. `path=Forward` 默认 stderr stamp
3. `configurePipeline(makeDeferred())` 切到 Deferred path：
   - 1 盏方向光下，截图 ≈ Forward 路径
   - 多盏方向光（Round 2，B7+）无视觉退化
4. `textured_material_draw_one_frame` 3 跑稳（§5.5 历史 flaky 站点严守）
5. `AYRenderer_Test` GBuffer / Lighting 相关 case 全绿
6. 公开头无新增 `bgfx::*` 类型
7. `Test_F1_LayoutDiag` sizeof(FrameContext) / sizeof(RenderScene) 不变 ── ABI 守门

## 六、文档

- 使用说明：[`docs/deferred-pass.md`](deferred-pass.md)
- 教训：[`docs/pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md)
- 项目级：`docs/execution-plan.md` §P5 / §5.3 / §5.4

## 七、§5.3 红线回查（每 PR 必检）

| 红线 | B 触碰面 |
|------|----------|
| RenderScene::Light struct | **禁** ── 多光走 ctx.lights（B7+）|
| FrameContext 加 GBuffer 槽 | **禁** ── PassExecContext::gbufferPass 借用指针 |
| `FrameContext&` 非 const | **禁** ── PassExecContext 仍 const FrameContext& |
| 默认挂 GBuffer enabled | **禁** ── 默认 Forward, Deferred opt-in |
| Lighting 后跑 ForwardOpaque 全量 | **禁** ── Pipeline 显式 skip |