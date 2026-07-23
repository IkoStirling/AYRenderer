# Pass 落地经验：GBuffer / Lighting (Deferred)

> **状态**：B0 ── 现场重置（docs only,0 代码），2026-07-22。  
> **目的**：把 Shadow Pass 集成时砸出来的成本与风险，收成一份 **GBuffer 专属** checklist，避免再趟一遍 Shadow 的 deprecated F1 SIGSEGV 那条旧路。
>
> 配套使用说明：[`deferred-pass.md`](deferred-pass.md)；shadow 同类参考：[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md)；项目级别的红线/隔离实验：[`execution-plan.md`](execution-plan.md) §5.3 / §5.4。
>
> **接入管线（一旦 B1–B6 ship）**：默认 Forward 5-pass 不变；Deferred path host 显式 `configurePipeline(makeDeferred())` 启用。新代码默认挂载一律走 disabled 闸（RenderPass 基类 `_enabled=true` + RenderPipeline::executeAll isEnabled 闸 ── 已 ship，见 §5 mirror）。

---

## 0. 为什么有这份文档

Shadow F1 SIGSEGV 历史（[`execution-plan.md`](execution-plan.md) §5.5）让我们付出了 4 个 commit 的 bisect 代价：
- 1）Light struct + FrameContext shadow 槽 + 默认 enabled → SIGSEGV
- 2）Shadow setEnabled(false) → 仍 SIGSEGV
- 3）从管线完全移除 Shadow → 仍 SIGSEGV（步 4 单独回滚后 3/3 PASS）

**结论**：big-bang 任意 PR 触碰以下任一红线组合 → 一致 SIGSEGV：
1. RenderScene 内多光源结构（Light struct / _lights / addLight）
2. FrameContext 加 GPU 句柄矩阵槽（shadowFboIdx / lightViewProj / lastFrameShadowFbo）
3. RenderPass::execute / FrameContext& 改非 const
4. 默认管线挂载一个 **enabled** 的新 Pass

**Deferred 极可能命中同型翻车点**，因为：
- GBuffer = MRT 句柄槽（**红线 #2**）
- LightingPass = 多光源数据（**红线 #1**）
- 默认管线必改（FO 让位给 GBuffer+Lighting）→ **红线 #4**
- 一次连续 PR 触及以上任二 → 重现 F1 翻车

**B0 的产出就是为 B1–B6 立 cutsheet，不重蹈 Shadow 4-commit bisect**。

---

## 1. 总原则（GBuffer / Lighting）

1. **先 Noop plumbing，再真 GPU**。Mirror Shadow：F1'（cut-1，仅私有 FBO，无 Light struct）→ F2（采样 plumbing）→ F3（skinned caster）→ §E5 默认 enabled。每个增量都 3 跑稳，再走下一刀。
2. **Producer 私有 / Consumer 只读**。Mirror `PassExecContext::shadowPass` 借用指针 → `PassExecContext::gbufferPass` 借用指针。LightingPass 通过借用指针读 attachments，**绝不**进 FrameContext，绝不进 PassExecContext sceneFbo 同字段写。
3. **管线顺序决定权 = RenderPipelineDesc**，不靠 Impl 默认 ctor 魔术。Forward vs Deferred 显式 path 选择（`RenderPath` enum），host 一处决定；默认 Forward 不变（§5.3 守）。
4. **每个新 GPU 资源都在 .cpp 内 ensure / destroy**，跟 Shadow 的 `_shadowFbo.ensure(adapter, size, stamp)`、`destroyResources(adapter)` 镜像对称。Resize() 重建必须走 Adapter ── 不要直接动 bgfx::handle。
5. **每个新 Pass 同时建：诊断 stderr + L0–L4 + 单测 stub**。Mirror `ShadowDiagnostics` + `Test_ShadowPass` + `Test_F1_LayoutDiag`。Logging 级别 0/1/2/3/4 同等级阶梯。
6. **改 shader 必 bump cache key / pipeline stamp**，同 lessons §1.5 + shadow-pass.md §环境变量表。
7. **生产路径 force-use `BGFXAdapter` 包装**：MTR attachments / uniform pad / view-frame buffer / vertex layout / capsule query 一律走 adapter (`capsTextureBlit/HomogeneousDepth` 等已 ship,见 BGFXAdapter.h)。

---

## 2. 推荐落地顺序（B1–B6，对应 [`execution-plan.md`](execution-plan.md) §P5）

| 阶段 | 做什么 | 触碰面 | 退出门 |
|------|--------|--------|--------|
| **B0** | **现场重置：docs only,0 代码**。本文件 + deferred-pass.md + execution-plan.md §P5 + roadmap | `docs/*` | 0 ABI 漂移；3 跑稳 → 写入附录 A |
| **B1** | `enum class RenderPath { Forward, Deferred }` + `RenderPipelineDesc::path` 字段 + `RenderPipeline` 仍跑 Forward | `AYRenderTypes.h` / `RenderPipeline.*` | 3 跑稳；默认 Forward 无变化 |
| **B2** | `GBufferPass` 空壳 / Noop 0-draw / `PassExecContext::gbufferPass` 借用指针（镜像 `shadowPass`） | `detail/GBufferPass.{h,cpp}` / `PassExecContext.h` / `AYRenderer.cpp` Impl ctor | 3 跑稳；ABI 新增 1 借用指针字段；FrameContext 0 改 |
| **B3** | Forward / Deferred path 显式切换 + `LightingPass` 空壳 / Noop 0-draw | `RenderPipeline.*` / `detail/LightingPass.{h,cpp}` / `AYRenderer.cpp` | 3 跑稳；host `configurePipeline(makeDeferred())` 切路径；默认 Forward 不变 |
| **B4** | GBuffer 真 MRT（NOOP 仍 0-draw,真 GPU 真画）：RT0 albedo / RT1 normal / RT2 motion / RT3 depth；**新加** `BGFXAdapter::createGbufferFrameBuffer` API（现网 `createFrameBuffer` 仅 1×color 或 1×color+1×depth,**不能**做 5-attach MRT ── 主人 B0.5 校正, 2026-07-22）| `BGFXAdapter.{h,cpp}` 新 API + `GBufferResources.{h,cpp}` + `GBufferPass.cpp` + Phoskia + tests + docs delta | 真 GPU Editor 截图：GBuffer 可视化（debug overlay） |
| **B5** | LightingPass 真光（NOOP 仍 0-draw,真 GPU 采 GBuffer）：单方向光（复用 `FrameContext::lightDirection`），共享 Shadow 借用句柄；vertex 全屏三角形 | `LightingPass.cpp` / 新 shader source | Editor 截图 parity vs Forward（一张图直接对画面） |
| **B6** | 默认 Forward 不变 / Deferred explicit opt-in / docs 收口 / execution-plan.md 附录 A 加 B0–B6 行 | `AYRenderTypes.h` / `RenderPipeline.*` / `docs/*` | Editor Play 默认 Forward 仍 ⮕ 零回归；Deferred opt-in 路径稳定 |

**显式推迟（不进 B0–B6）：**
- 多光源 DataSource（**B7+ Round 2**，**必须**走 ctx.lights 借用指针，绝不进 FrameContext 绝不进 RenderScene Light struct）
- 透射 / 皮肤 / 半透明等 "still Forward" 子集
- 环境光照 / IBL（独立 P-Roadmap item,不是 Deferred Pass 的事）
- 多 GBuffer alias（per-material layout）─ v1 不做,albedo+normal+motion+depth 锁死
- Deferred MSAA resolve（MSAA 在 forward 已做,deferred 分辨率 = sceneFbo 大小,主 RT 已是 viewport）
- VSM / ESM / PCSS（Shadow 仍未做,Deferred 不再次开窗）

---

## 3. 复用既有 utilities（直接用，不重造）

| 已有 | 用法 |
|------|------|
| `BGFXAdapter::createFrameBuffer(w,h,RGBA8,withDepth)` | GBuffer MRT 主 RT（RGBA8 4 slot）─ 直接用 |
| `BGFXAdapter::createDepthOnlyFrameBuffer(w,h)` | GBuffer depth-only attachment（若主 RT 不绑 depth） |
| `BGFXAdapter::setViewFrameBuffer(viewId, fbo)` | LightingPass / GBufferPass view 锁定 |
| `BGFXAdapter::setViewClearRaw(viewId, ...)` | GBuffer 初始化 clear（区别于 sceneFbo 用 rgba 0x191a1cff,GBuffer 默认 clear=0 / 深度=1） |
| `BGFXAdapter::setStateOpaque` / `setStateAlphaBlend` / `setStateDepthOnlyWrite` / `setStateDepthTestAlways` | 4 个 state 预设（P6.5 ship） |
| `BGFXAdapter::vertexLayoutPosUv()` | LightingPass 全屏三角形 layout preset（P6.5 ship） |
| `BGFXAdapter::capsTextureBlit/ReadBack/HomogeneousDepth` | Motion/RT copy decide |
| `PassExecContext::shadowPass = const ShadowPass*` | `PassExecContext::gbufferPass = const GBufferPass*` 镜像（B2） |
| `RenderPipeline::executeAll` isEnabled 闸 | 新 pass 默认 disabled 由主机手动 enable（B3 新增 path 选择后） |
| `ForwardOpaquePass::flushMaterial` 的 trySetUniform helpers | GBuffer MRT fragment 不需要这些（LightingPass 全屏三角形 + sample） |
| `ShadowDepthCodec` ndc01 / pack / compare | **仅供 ShadowPass**。GBuffer 深度走 §5.2 `RT3 D24S8` 硬件 depth ── Shadow R8 复刻是 `ShadowMapResources` 的 workaround（lessons §3.6 + shadow-pass.md L113），**不**套到 GBuffer。 |
| `FrameContext::lightDirection` / `lightColor` | LightingPass B5 复用 1 盏方向光 ── 严守 §5.3（不进 Light struct） |

---

## 4. 红线守门 ── GBuffer 触碰矩阵

### 4.1 §5.3 禁止同一 PR 引入

| 禁止 | B 触碰 |
|---|---|
| RenderScene Light struct / addLight / _lights | B5 1 盏方向光走 FrameContext::lightDirection 已有 primitive ── **不进 Light struct**；B7+ 多光源必须新 DataSource（ctx.lights 借用指针） |
| FrameContext 加 GBuffer FBO 槽 / lighting 矩阵槽 | **禁** ── GBuffer attachments 由 GBufferPass 私有，LightingPass 通过 `ctx.gbufferPass` getter |
| RenderPass::execute / FrameContext& 改非 const | **禁** ── PassExecContext 仍 const FrameContext 引用 |
| Lighting 后用 ForwardOpaque 重画不透明 | **禁** ── Deferred path 时 FO/Trans skip，B3 pipeline 显式 skip |

### 4.2 B0–B6 PR 切片硬约束（每 PR ≤ 8 文件）

| 红线 | 应对 |
|---|---|
| 任一 PR 同时引入 RenderScene::Light + FrameContext::gbufferFbo + 默认挂 GBuffer + 真 GPU 画 | **绝对禁止** ── Mirror Shadow F1 SIGSEGV 步 1–3，全 SIGSEGV at textured_material_draw_one_frame（§5.5 历史） |
| B4 一次 推 4 个 MRT attachment + depth + Phoskia 多 output 编译调试 | 拆：B4a MRT attachment alloc / B4b Phoskia receiver 编写 / B4c Phoskia↔.sc 对齐 |
| B5 一次 推 全屏三角形 + GBuffer sampler + shadow reuse + tonemap | 拆：B5a 全屏三角形 plumbing / B5b GBuffer sample / B5c shadow reuse（影子 ≠ 平行工作） |

### 4.3 触碰面 ≤ 8 文件（每个 PR）

| PR | 文件数估算 | 说明 |
|---|---|---|
| B0 | 4 docs | 零代码 |
| B1 | 2–3 | `AYRenderTypes.h` / `RenderPipeline.{h,cpp}` / 1 test |
| B2 | 5–7 | `GBufferPass.{h,cpp}` / `PassExecContext.h` / `AYRenderer.cpp` Impl ctor / `RenderPipeline.{h,cpp}` / 1–2 tests |
| B3 | 4–6 | `LightingPass.{h,cpp}` / `RenderPipeline.{h,cpp}` / `AYRenderer.cpp` / 1–2 tests |
| B4 | 6–8 | `GBufferResources.{h,cpp}` + `GBufferPass.cpp` + Phoskia 源 + 1–2 tests + docs delta |
| B5 | 6–8 | `LightingPass.cpp` + Phoskia 源 + 2 tests + docs delta |
| B6 | 3–5 | `AYRenderTypes.h` 工厂方法 / `RenderPipeline.*` / docs |

任一超 8 → 必须再切，绝不堆。

---

## 5. Host / Pipeline 契约

### 5.1 View 分配（**必读 ── 与代码现状一致,迟到 B1/B3 直接撞 Shadow**）

代码基线（2026-07-22 E5 ship 后）7 个 view id 全部被占用 ── 新加 Deferred **必须用新 view id**，**禁止复用**：

| view id | 用途（Forward path 现行） | 出处 |
|---------|--------------------------|------|
| **0** | full-window clear | `beginCompositeFrame` 全窗 clear |
| **1** | Shadow caster（depth FBO 写入）| `ShadowPass::kShadowViewId = 1`（`ShadowPass.h:33`）|
| **2** | Shadow resolve blit（color RT → sampleable tex）| `ShadowPass::kShadowResolveViewId = 2`（`ShadowPass.h:34`）|
| **3** | ForwardOpaque（FO 用 `ctx.viewId`,Renderer composite 推 3）| `AYRenderer.cpp:380,384` |
| **4** | Transparent | `TransparentPass::kTransparentViewId = 4`（`TransparentPass.h:37`）|
| **5** | PostProcess blit-to-backbuffer | `PostProcessPass::kBlitViewId = 5`（`PostProcessPass.h:68`）|
| **6** | UI chrome | `UIRenderBackend::kViewId = 6`（`AYUIRenderBackend.h:40`）|

**Deferred path 分配（B3 切换时钉死）**：

| Path | view 0 | view 1 | view 2 | view 3 | view 4 | view 5 | view 6 | view **7** (新 B4) | view **8** (新 B5) |
|------|--------|--------|--------|--------|--------|--------|--------|------------------|------------------|
| **Forward（默认,现网）** | full clear | Shadow caster | Shadow resolve | FO | Transparent | PP blit | UI chrome | ── | ── |
| **Deferred（opt-in B3+）** | full clear | Shadow caster | Shadow resolve | (FO skip) | Transparent | PP blit | UI chrome | **GBuffer MRT** | **Lighting** |

**铁律**：
- ❌ **绝不**用 view 1/2/3/4/5/6 任何槽位给 GBuffer/Lighting —— 已被 Forward 钉死,**撞 Shadow = 全黑**。
- ❌ view 2 是 Shadow resolve blit ── B0 初稿曾错写 `kGBufferViewId=2`，**必弃**。
- ✅ GBuffer 用 **view 7**（B4 引入），Lighting 用 **view 8**（B5 引入）。
- ✅ Forward path 下 **FO 完全 skip**（Pipeline 不挂 `RenderPassSlot::ForwardOpaque`）── view 3 在 Renderer 仍被拥有但 **0 draw**。
- ✅ Transparent（view 4）仍画 ── Alpha 物体在 LightingPass 输出后 scene 上合成 ── 这是 §3 漏写的关键:P2 已有 ctx.sceneFbo closure，LightingPass 写 LightingOutput FBO → Transparent 仍写 LightingOutput FBO 同 view 5 上 ?

⚠ **关键约束：LightingPass vs Transparent 顺序** ── 见下表：

```
L3.1 (B5 才填) — Deferred path 实际的 view 4 / view 5 谁先跑谁后跑
   候选 A：LightingPass(view 8) → Transparent(view 4) → PP(view 5)
      → Transparent 写 LightingOutput FBO；PP 采 LightingOutput → backbuffer
      → Risk：Lighting 跟 Transparent 是否共享同一个 LightingOutput FBO？
   候选 B：Transparent(view 4) → LightingPass(view 8) → PP(view 5)
      → Transparent 写 sceneFbo（原本的 FO+Trans 共享 sceneFbo，FO skip 后只剩 Trans）
      → Risk：Transparent 用什么 depth？GBuffer 没绑 depth 给它（depth 留 Lighting sample 用）。
   → **B5 ship 前必须选定并写入 §P5.2 decision table**。
   → **B0 留 OPEN**，由 B3 + B5 双 PR 落实。
```

约束重申：
- 默认 Forward **0 改动**，view 分配保持现网。
- PostProcessPass source-FBO 在 Deferred path 必须用 **`ctx.gbufferPass->lightingOutputFbo()`** 借用指针取；当前是 `ctx.sceneFbo` fallback。**B6 必须改 PP source 选择优先级**。

### 5.2 GBuffer attachment 集合（B4 lock）

| Slot | 格式 | 内容 | 备注 |
|------|------|------|------|
| RT0 | RGBA8 | albedo（base color RGB, alpha = roughness / reserved）| B4a 锁，v1 不改 |
| RT1 | RGBA8 | world-space normal（xyz + motion 槽占位）| B4a 锁 |
| RT2 | RGBA8 | motion vector（xy, zw reserved）| B4a 锁 |
| **RT3 (depth)** | **D24S8** | **硬件 depth attachment（独立 depth 槽,RT0–2 不绑 depth）| **B4 lock** — 这是 **标准 bgfx 模式 + 硬件 depth**；**绝不**复刻 Shadow R8 workaround（lessons §3.6 + shadow-pass.md L113 是 **ShadowMap 专用**，不套到 GBuffer）|

后续 alias（spec / sssMask / velocity / etc）**v1 不开**,B4 锁死。

### 5.2.1 GBuffer MRT helper（B4 必须新加 API，现网不够）

主人反映（B0.5 校正，2026-07-22）：

**现网 `BGFXAdapter::createFrameBuffer` 实际能力**（`BGFXAdapter.h:97,105,112`）：
- `createFrameBuffer(uint16, uint16, TextureFormat, withDepth)` ── 单 color（或 + 单 depth）。
- `createColorDepthFrameBuffer` ── 1× color + 1× depth（`BGFXAdapter.h:112`，P2 closure 已 ship,用于 FO+Trans 共享 sceneFbo）。
- `createDepthOnlyFrameBuffer` ── 仅 depth。

→ **现网 0 个 MRT helper**。4-slot RGBA8 + D24S8（= 5 attachments）B4 必须加新 API：

```
BGFXAdapter::createGbufferFrameBuffer(uint16_t width, uint16_t height);
  // returns bgfx::FrameBufferHandle with:
  //   attach[0] = RGBA8 albedo   (TextureFormat::RGBA8)
  //   attach[1] = RGBA8 normal   (TextureFormat::RGBA8)
  //   attach[2] = RGBA8 motion   (TextureFormat::RGBA8)
  //   attach[3] = D24S8 depth    (TextureFormat::D24S8 + stencil)
  // Implementation: 走 bgfx::createFrameBuffer(width, attachCount, attachmentHandles)
  //   或 走 bgfx::createFrameBuffer 后调 attach(ids) — 见 bgfx::FrameBufferHandle API
  // **B4 必须 ship 此 API + Test_BGFXAdapter_MRT 守门**（attach 数 + format + viewport）.
```

**B4 触碰面加 1**: `BGFXAdapter.h` + `BGFXAdapter.cpp` 新 API + test。

> ⚠ 若将来要 RGBA16F normal 或 RG16 motion 等不同 format ── `createGbufferFrameBuffer` 加参数。**v1 锁 4 RGBA8 + 1 D24S8**。

### 5.3 Lighting 输出

| 形状 | 备注 |
|------|------|
| 全屏三角形（shared layout）| 复用 PostProcessPass 的 vertexLayoutPosUv（P6.5 ship） |
| Frag out:rgba | 半精度等后续讨论。v1 = RGBA8 |
| 1 盏方向光（FrameContext::lightDirection / lightColor）| B5 lock |
| 多光源 DataSource | **B7+** ── 走 ctx.lights 借用指针,**不进 FrameContext** |

### 5.4 Material 绑定契约

跟 shadow receiver 同样的 `PassExecContext::gbufferPass` getter 模式：
- GBufferPass 持 RGBA8 4-slot attachments + 1 depth attachment。
- LightingPass 仅持有借用指针取 attachments ── **生产端私有**,消费端只读。
- 公开 API 钉 `gbufferPass()->albedoTexture()` / `normalTexture()` / `motionTexture()` / `depthTexture()` / `lightingOutputFbo()` ── **不暴露 bgfx handle 到公开头**,跟 shadowFbo 镜像（PublicHeaderSurface 测试已守门）。

---

## 6. 调试手法（可直接抄）

1. **双路径 A/B**（Mirror Shadow F1）  
   Forward：默认（`makeDefault()`）  
   Deferred：`configurePipeline(makeDeferred())`  
   严格 PowerShell（不要用 cmd `SET`）。

2. **GBuffer 可视化**  
   debug overlay / 4 张图（albedo / normal xy / **depth D24S8 取 linearized 灰度** / motion xy）。Noop 路径返回空 PNG，正常。  
   ⚠ GBuffer depth 是 **D24S8 硬件 depth**，**不**是 R8 复刻（ShadowMapResources 的 R8 workaround 仅服务于 Shadow caster R8 复刻 ── 见 shadow-pass.md L113 + lessons §3.6）；debug overlay 取 depth 必须经 linearize helper，不能直接当作 color RT sample。

3. **单 Pass 探针**  
   - 仅 GBuffer 坏：attachments 错位（view rect 大小不一致） / MRT slot attach 顺序乱。
   - 仅 Lighting 坏：GBuffer 报正确但画面黑 ── sampler binding 错 / vertex layout 错。
   - 视野错位：clip.xy 未透视除 / Y flip / LVP 矩阵顺序错（已 shadow F1 教训）。
   - 光黑：lightDir uniform pad 错（lessons §3.1 vec4 ABI）。

4. **诊断开关**（mirror Shadow L0–L4）  
   `AY_DEFERRED_LOG` / `AY_DEFERRED_DEBUG` / `AY_DEFERRED_MRT_FALLBACK` ── B6 收口,先 stub。

5. **单测钉子**  
   source 契约 / emit 契约 / sizeof guard (B2 `PassExecContext` 加字段后,FrameContext size 不变,任何 ABI 漂移报警) / Noop plumb 路径。

---

## 7. 问题→结论速查（preset，等真踩坑后填）

| 现象 | 根因（猜想） | 结论 |
|------|------------|------|
| 画全黑 | GBuffer attachments 未 attach / clear 错 | MRT slot 顺序查 + `ensureGbufferResources` 探针 |
| 视野错位 | GBuffer view rect 与 viewport 不一致 / LightingPerspective 矩阵错 | viewportW/H 走 ctx.viewportWidth/Height,不让 GBuffer 自己取 |
| Phoskia 多 frag output 编译失败 | converter 不支持 `out vec4` 多次输出 → B4b 须 hand `.sc` 黄金前置 |
| LightPass 跟 Forward 不一致 (parity 差) | 矩阵顺序、LVP、light normalization 等 | 钉 FrameContext::lightDirection 量级,Editor 截图精确比对 |
| SIGSEGV at `textured_material_draw_one_frame` | **§5.5 / 红线 #2 #4** | 立刻停手:F1 教训镜像 → 回滚到 B3,3 跑稳才进 B4 |

---

## 8. 新 Pass 开干前的复制清单

复制下面一节到新 PR 的 commits / `docs/<pass>.md`：

- [ ] B0 docs ship + 3 跑稳  
- [ ] B1 RenderPath enum 引入 + Pipeline 仍 Forward + 3 跑稳  
- [ ] B2 GBufferPass 空壳 + PassExecContext::gbufferPass 借用指针 + Noop 0-draw + 3 跑稳  
- [ ] B3 Forward / Deferred path 显式切换 + LightingPass 空壳 + Noop 0-draw + 3 跑稳  
- [ ] B4 GBuffer 真 MRT + 测试钉 debug overlay + Editor 视觉验  
- [ ] B5 LightingPass 真光 + 1 盏方向光 parity vs Forward  
- [ ] B6 默认 Forward 不变 + docs 收口 + 附录 A 加 B0–B6  
- [ ] B7+ 多光源 DataSource 走 ctx.lights 借用指针（**不**进 FrameContext / RenderScene）  
- [ ] 公开头 (include/*.h) 无 `bgfx::` / 公开 API 增 surface ≤ 3 (RenderPath + Deferred opt-in factory + 0 setter 视情)  
- [ ] PassExecContext sizeof 不变（仅加 1 借用指针字段）or 显式记录新 size 与 §5.4 一致 test 钉  
- [ ] FrameContext sizeof 0 改（MUST）  
- [ ] RenderScene.sizeof 0 改（MUST）  
- [ ] RenderPass::execute 签名 0 改（MUST）  

---

## 9. 相关代码锚点（预设,后续更新）

| 主题 | 位置 |
|------|------|
| Shadow 借用指针模式参考 | `src/detail/PassExecContext.h:125` (`shadowPass`) |
| Shadow 私有 FBO 模式 | `src/detail/ShadowMapResources.{h,cpp}` ── GBufferResources 镜像 |
| Shadow 矩阵 builder | `src/detail/ShadowLightMatrix.{h,cpp}` / `ShadowMatrixBuilder.{h,cpp}` ── GBuffer identity 不需,LightingPass 走 ctx adapter 取 Projection/View |
| Phoskia receiver contract | `include/AYShadowReceiverContract.h` ── LightingPass fragment 另起 |
| Shadow 借用 pass getter 消费者 | `src/detail/RenderPass.{h,cpp}` (`tryBindShadowSampler`) ── GBuffer pass getter stub 直接走 `ctx.gbufferPass` |
| Adapter API | `src/detail/BGFXAdapter.{h,cpp}` ── 已 ship 多 cap wrapper / state preset |
| 公开头守门 | `unittest/Test_PublicHeaderSurface.cpp` ── GBufferPass 类加入 sizeof+符号断言 |

---

## 10. 刻意未做（避免后续 PR 误抄）

- **未**让 GBufferPass 直接拥有 TextureHandle 给 host（与 ShadowPass::shadowFbo 镜像,公开头通过 getter 出 handle,公开 API 不留 bgfx handle 给 TU）。
- **未**让 LightingPass 在执行前重新计算 Shadow（共用 ShadowPass 只读借用）。
- **未**在 FrameContext 写 GBuffer 句柄（红线 #2;GBuffer 私有 / ctx 借用）。
- **未**在 RenderScene 加 Light struct（红线 #1;多光**走 ctx.lights 借用指针** ── 推迟到 B7+）。
- **未**默认挂 GBuffer + 启用（红线 #4;默认 Forward,Deferred opt-in 走 path enum）。
- **未**让 LightingPass 跑完后还跑一遍 ForwardOpaque（§5.3 §P5.4 强禁;FO/Trans 在 Deferred path 直接 skip）。
- **未**跨 backend IBL / SSR / SSAO 等高级光照（独立 roadmap,不是 Deferred Pass 自身的事）。

---

## §P5.5 — 光类型补全 (A → B → C) cutsheet

### A — Unified `Light` POD + `LightType` enum (2026-07-23, ships)
- Replaces pre-A `DirectionalLight` POD with a tagged-union `Light` POD
  carrying `LightType { Directional=0, Point=1, Spot=2 }`. `DirectionalLight`
  retained as a `using DirectionalLight = Light;` alias for source-compat.
- UBO field rename: `Lights.dirs[8]` → `Lights.record[8]` (`xyz = vector`,
  `w = float(LightType)`). CPU-side pack fans out per `LightType` so the
  receiver math stays byte-equivalent for Directional-only hosts.
- A ships the cut-shape: **receiver path unchanged, 9-tap key-shadow
  unchanged, host behaviour 0 diff** — every default-constructed `Light`
  is `type=Directional` so the change is naming-cleanup only.
- cache-key: `lighting_v16_b5p5_worldpos_rgba16f` → `lighting_v18_b5p5a_light_pod`.
- Test_B7 + Test_B5p5 mirror drift fixed at the same time (v10 / v16 → v18).
- 红线全守: `RenderScene::Light` 永退 / FrameContext 0 grow / `RenderPass::execute`
  签名 0 改 / Forward host 0 行为变化 / 公开头 0 加 `bgfx::` / 文件 ≤ 8。
- Future B/C budget:
  - B widens UBO with `vec4 params[8]` + `vec4 spotDir[8]` and adds Phoskia
    attenuation + spot cone math (`Lights.record[i].w = float(type)` already
    ship-side from A — no FS rewrite needed for the gate).
  - C wires per-light shadow via `PassExecContext::perLightShadows` borrowed
    ptr + dual-FS program (K3: `count==0` ⇒ B program = 9-tap key-only,
    host pays 0 incremental cost).

---

## §Skybox0 — Equirect Skybox Pass (2026-07-23, ships)

**主路线决定:** MVP ship equirect 2D panorama,**预留 cubemap 扩展位**
(`enum SkySourceKind { Equirect=0, CubeMap=1 }` enum 占位;CubeMap 路径不 ship
code,留 §Skybox0-B 接 samplerCube)。

### 设计契约

| 维度 | 决策 |
|---|---|
| **数据源** | `ayt::render::SkySource` host POD ── `kind` (enum) + `equirect` (TextureHandle) + `cubeReserve` (uint64_t reserved);`isActive()` ⇒ `kind==Equirect && hasEquirect()` |
| **borrowed ptr** | `PassExecContext::skySource` + `PassExecContext::skyboxPass` ── 2 trailing defaults,17→18→19-field brace-init 兼容 |
| **Pipeline slot** | `RenderPassSlot::Skybox = 1`, 在 Shadow 之后 GBuffer 之前;`makeDeferred()` 7-slot;`makeDefault()` Forward 不含 ── 红线 #4 (Forward host 0 行为变化) |
| **几何** | 全屏三角形 (复用 `kFullscreenTriangle` pattern,no DrawItem loop) |
| **RT** | 独立 `skyFbo` RGBA8 viewport size,withDepth=false (sky 在无限远);LightingPass 多 sample 一次合成 |
| **view id** | **6** ── cutsheet §5.1 lock 表新增;预 §Skybox0 reserved 6 现在 claim |
| **合成** | `mix(skyColor, lit, coverage)` ── sky 只在 lit ≈ 0 的区域当 backdrop,geometry 保持原色 |

### 文件触碰 (10 文件,微超 ≤ 8 红线 2 个 ── 主人拍板)

| # | 文件 | 改动 |
|---|---|---|
| 1 | `include/AYRenderScene.h` | `enum SkySourceKind` + `struct SkySource` POD |
| 2 | `include/AYRenderTypes.h` | `RenderPassSlot::Skybox` enum |
| 3 | `include/AYRenderer.h` | `setSkySource` + `skySource()` getter |
| 4 | `src/detail/SkySource.h` (新) | DS alias (mirror PerLightShadowDS.h) |
| 5 | `src/detail/SkyboxPass.h` (新) | RenderPass subclass header |
| 6 | `src/detail/SkyboxPass.cpp` (新) | Phoskia source + ensure/execute |
| 7 | `src/detail/PassExecContext.h` | `skySource` + `skyboxPass` borrowed ptrs |
| 8 | `src/detail/LightingPass.h/.cpp` | `_gbufferSkyRt` field + FS `texture2d gbufferSky` + `vec4 skyMix` + `mix(skyColor, lit, coverage)` |
| 9 | `src/AYRenderer.cpp` | `makeDeferred` 7-slot + `makePassForSlot` factory + `setSkySource` impl + `Impl::_skySource` field + `render()` ctx fill + `setOutputSize` 广播 |
| 10 | `unittest/Test_Skybox0.cpp` (新) + `unittest/Test_B3_LightingForwardDeferred.cpp` (修正 deferred passes.size 6→7) | 8 new cases + 1 updated |
| 11 | `docs/pass-lessons-from-deferred.md` | 本节 |

### K1 关键 invariants

1. **MVP A 只 ship equirect 路径** ── `SkySourceKind::CubeMap` 是 enum value 但
   no code path 接 cubemap sampler。host 设 CubeMap → SkyboxPass early-return 0
   (no crash)。
2. **Forward host 0 行为变化** ── SkyboxPass 仅在 `RenderPassSlot::Skybox` 显式
   mount 的 Deferred pipeline 跑;`makeDefault()` 5-slot Forward 不含 Skybox ──
   Test_ForwardOpaque + Test_ForwardOpaque_BlendSkip_P0_4 0 触动。
3. **SkyboxPass Noop 路径** ── `isNoopBackend() || !isInitialized()` ⇒ 0;
   `ctx.skySource == nullptr` ⇒ 0;`kind != Equirect` ⇒ 0;`!hasEquirect()` ⇒ 0。
4. **LightingPass sky-blend 默认 intensity = 1.0** ── `skyMix.x = 1.0` 永远
   upload;host 后续可 `setMaterialVec3(material, "skyMix", 0.5)` 调低。
5. **gbufferSky sampler 默认 = 不绑** ── `ctx.skyboxPass == nullptr` ⇒ 不绑
   sampler;Phoskia FS `sample(gbufferSky, baseUv)` 拿到 0 (Phoskia UB 行为,
   bgfx 实际给 black) ── `mix(black, lit, 1) = lit` ── 跟 B5.5 行为 1:1。
6. **Test 守门 invariant** ── `Test_B5p5::b5p5_key_light_shadow_only_fill_unshadowed`
   在 §Skybox0 后仍绿 ── 因为 LightingPass 9-tap shadow 路径不变 + sky-blend
   只在 lit 接近 0 时覆盖 (默认 shadow-lighting 把 ground 渲亮,sky 不染色)。
7. **§Skybox0 不引入 IBL** ── ambient term 仍是 `vec3(0.1, 0.1, 0.1)` flat;
   IBL 留给 future。

### 复用既有 utilities

| 已有 | 用法 |
|---|---|
| `LightingPass::kFullscreenTriangle` pattern | SkyboxPass copy 一份 (duplicate-constant 习惯) |
| `BGFXAdapter::createFrameBuffer(w, h, RGBA8, withDepth=false)` | SkyboxPass FBO ensure |
| `BGFXAdapter::createVertexBuffer` + `vertexLayoutPosUv()` | SkyboxPass fullscreen triangle |
| `LightingPass::setOutputSize` + `ensure` + `ensureProgram` + `destroyResources` | SkyboxPass 镜像 |
| `PassExecContext::shadowPass / gbufferPass / lightingPass` borrowed ptr | `skyboxPass` 镜像 |
| `RenderPipelineDesc::makeDeferred()` + `makePassForSlot` 工厂 | Skybox slot 插入 |
| `Renderer::setSceneLights` 公开 setter pattern | `setSkySource` 镜像 |
| `BGFXAdapter::getFboAttachment` | `SkyboxPass::skyRt()` 镜像 |

### View-id 锁表更新 (cutsheet §5.1)

| view | 用途 | 备注 |
|---|---|---|
| 0 | backbuffer / clear | default |
| 1 | ShadowPass caster | pre-B5.5 |
| 2 | ShadowPass resolve | pre-B5.5 |
| 3 | ForwardOpaque / Transparent | shared |
| 4 | PostProcessPass (forward path) | pre-B5.5 |
| 5 | UI | pre-B5.5 |
| **6** | **SkyboxPass (sky backdrop)** | **§Skybox0 新增 (2026-07-23)** |
| 7 | GBuffer MRT | B4 |
| 8 | LightingPass fullscreen | B5 |
| 10 | PostProcessPass deferred blit | B6 |

### 不在 §Skybox0 scope (deferred 到 future)

| Item | Reason |
|---|---|
| `CubeMap` sampler path | 主人明示「先 ship equirect, cubemap 未来」;`SkySourceKind::CubeMap` enum 占位但 no code path 接 |
| IBL (ambient cube / irradiance / radiance) | MVP 不需 ambient/diffuse env light,纯 backdrop |
| Skybox 滚动 / 旋转动画 | owner 没要求 |
| Per-pixel depth-test (sky depth = 1 / far) | fullscreen + DEPTH_TEST_ALWAYS 够用 |
| Phoskia dir calc (lat/long → 3D) for IBL | MVP FS 直接 sample,不 calc dir |
| SkyboxPass depth 写 | sky = at infinity, no depth write (cutsheet 同意) |
| 双 cubemap (左右眼 VR) | VR cut,not in MVP |
| Fog (sky tint by fog) | post-§Skybox0 |

---

## §P5.5 B — Point + Spot light types (2026-07-23, ships)

Post-A ship (1172/1172 → post-§Skybox0 1231/1231), B 刀接通
Point + Spot 真光路径 ── host 设 `Light{ type=Point/Spot, ... }`
后,LightingPass FS 用正确 attenuation + cone math 出图。

### 设计契约

| 维度 | 决策 |
|---|---|
| **Light POD widen** | 加 `float range` + `float intensity` + `float coneCosInner` + `float coneCosOuter` + `FVector3 spotDirection` (5 个新字段)。`sizeof(Light)` 从 ~40B 升到 ~68B。`static_assert` ≤ 64 → ≤ 96 (留 headroom 给 future Spot 扩展)。 |
| **UBO 宽** | `uniformblock Lights { vec4 dirs[8]; vec4 colors[8]; vec4 params[8]; vec4 spotDir[8]; } binding 0` ── 4 vec4 arrays × 8 lights × 16B = 512B。`params[]` = (range, intensity, coneCosInner, coneCosOuter);`spotDir[].xyz` = Spot direction。 |
| **dual-path upload 处理** | **砍** ── 只留 field-split (`setUniform` per-array × 4 calls)。删 `setUniformBlock` fallback (A 的 dual-path 在 4-array 上会变 2^4 组合)。 |
| **FS per-type 分支** | 8 unrolled `if / else if` 链,按 `dirs[i].w = float(LightType)` 分 3 路 (Directional < 0.5, Point < 1.5, Spot otherwise)。Phoskia `fn` helper 不依赖 ── 保持 converter 路径简单。 |
| **Shadow 仍只乘 lights[0]** | key light 唯一;fill/rim 1..7 不 shadow。B 不改 shadow 路径,Per-light shadow = §P5.5 C 范围。 |
| **default byte-equivalent invariant** | 默认构造 `Light{ type=Directional, ... }` 仍跟 pre-B 完全一致 ── 新字段都 default (range=0, intensity=1, coneCos=0, spotDirection=default);FS Directional branch 忽略 `params[]` / `spotDir[]`。 |

### Bug fix #3 — Test_B5 cache-key 自比自 false green

**真 Bug**(不是 silent red ── 是 false green):
- Pre-B, `kLightingCacheKey` 是 `LightingPass.cpp` 的 `static constexpr`,无法外部访问。
- `Test_B5_LightingDirectional.cpp::b5_lighting_cache_key_and_build_stamp_pinned`
  只能 self-compare (`mirror == mirror` = "mine"),drift 检测失效。
- 自 B7 bump (v3 → v10 → v16 → v18 → v20) 一直在 silent drift,
  test 仍然 pass ── false green。

**修法**:
- `LightingPass.h` 加 `extern const char* const kLightingCacheKeyCStr;`
  (其定义在 `LightingPass.cpp`)。
- `Test_B5` / `Test_B5p5` / `Test_B7` 都改:
  `CHECK(localMirror == kLightingCacheKeyCStr)` 而不是 self-compare。
- 任何 `kLightingCacheKey` 漂移立刻 fail,不再 false green。

### 触碰面 (6 文件, ≤ 8 红线内)

| # | 文件 | 改动 |
|---|---|---|
| 1 | `include/AYRenderScene.h` | Light POD widen + `static_assert ≤ 96` + `Light::point()` / `Light::spot()` factory |
| 2 | `src/detail/LightingPass.h` | `extern const char* const kLightingCacheKeyCStr;` |
| 3 | `src/detail/LightingPass.cpp` | `kLightingCacheKey` v20 → v21 + `lightsBlock[64]` → `[128]` (512B) + 删 dual-path fallback + Phoskia FS 重写 (8 段 per-type 分支) + `kLightingCacheKeyCStr` 定义 |
| 4 | `unittest/Test_B5_LightingDirectional.cpp` | cache-key mirror v3 → v21 + `CHECK(localMirror == kLightingCacheKeyCStr)` + 删 `shadowMap` forbidden + mirror FS 改 (4-array UBO + per-type 分支) |
| 5 | `unittest/Test_B5p5_LightingShadow.cpp` | cache-key v20 → v21 + extern live + 删 `gbufferDepth` sampler (B 后从 FS 退役) + substring pin 加 `Lights.params[0]` / `Lights.spotDir[0]` / `let keyContrib =` / `let fillContrib =` + shadow-key-only-fill-unshadowed case 改打 fillContrib 分支 substring |
| 6 | `unittest/Test_B7_MultiLightAccumulation.cpp` | cache-key v20 → v21 + extern live + mirror FS 改 (4-array + per-type branch) + `b7_lights_block_layout_dirs_then_colors` 改 512B + **5 新 case** (Point factory / Spot factory / size assert / 4-array layout / live cache-key) |
| 7 | `docs/pass-lessons-from-deferred.md` | 本节 |

### K1 (P5.5 B) 关键 invariants

1. **dual-path 砍 → 单一 field-split 路径** ── 4 `setUniform`
   calls × 128B each;删 `setUniformBlock` fallback。**未来
   D3D 3-run 验证后再迁回 UBO 单路径**。
2. **Light POD ≤ 96** ── static_assert 从 ≤ 64 升 ≤ 96,实际 68B。
3. **default `LightType = Directional` byte-equivalent** ── 新
   字段全 default,FS Directional branch 走 `ltype < 0.5` 时
   忽略 `params[]` / `spotDir[]`,跟 pre-B A ship 行为 1:1。
4. **Shadow 只乘 lights[0]** ── key-only shadow multiply,在
   `keyContrib` 的 Directional 分支内 (`* shadowKey *`)。fill/
   rim 1..7 永不乘 shadow。
5. **`b5p5_key_light_shadow_only_fill_unshadowed` 仍绿** ──
   shadowKey 只乘 lights[0] 不变。
6. **§Skybox0 路径不动** ── `mix(skyColor, lit, coverage)` +
   `gbufferSky` sampler + `skyMix.x = 1.0` 全保留。
7. **3-array unroll** ── 8 段 `if/else if` inlined,per-light
   分支不依赖 Phoskia `fn`(降低 converter 路径风险)。
8. **Forward host 0 行为变化** ── A ship 已 ship 8 unroll
   dir-only;B 后 Forward host (默认 setDirectionalLight →
   FrameContext single-light fallback) FS 走 `ltype == 0.0`
   (Directional),`params[]` 全 0,行为等价。

### 复用既有 utilities

| 已有 | 用法 |
|---|---|
| `Light::directional()` factory (AYRenderScene.h:147) | `Light::point()` + `Light::spot()` 镜像 |
| `SceneLights::add(const Light&)` (AYRenderScene.h:220) | B 不改 ── 已能装新 POD |
| `PassExecContext::sceneLights` borrowed ptr (PassExecContext.h:219) | B 不加新 field ── 复用 |
| `LightingPass::kFullscreenTriangle` + `kFullscreenIndices` | B 不改 ── 几何不变 |
| `LightingPass::tryBindShadowSampler` (LightingPass.cpp:797) | B 不改 ── shadow 仍只乘 lights[0] |
| `_program.getUniformBinding / setUniform` (GpuResources pattern) | B 加 2 个 calls (params + spotDir) |
| `B7+` 8 tap unroll + `Lights.dirs[i].w = float(LightType)` (A ship) | B 在 FS per-type 分支内 dispatch `ltype` ── A 已 ship 死代码, B 激活 |

### 不在 §P5.5 B scope (deferred 到 future)

| Item | Reason |
|---|---|
| Per-light shadow (Point/Spot 各自 shadow map) | §P5.5 C 范围 ── cutsheet 已预留 PassExecContext::perLightShadows borrowed ptr 位 |
| IBL (ambient cube / irradiance / radiance) | **§P5.5 D ships ambient cube lookup MVP (2026-07-23);** radiance / prefilter / roughness-driven LOD = §P5.5 D-radiance future |
| Spot light shadow | 跟 Point shadow 一并进 §P5.5 C |
| Phoskia `fn` 性能优化(避免 fn call 边界) | Phoskia converter 自决;若不支持,降级到 8 段 inlined if/else if |
| `setUniformBlock` 单路径迁回 | D3D 3-run 验证 B ship 稳后,future cut 再迁 |
| Light POD 字段重排 (把 type 字段挪到底) | 当前布局已 4-byte align,没必要 |

---

## §P5.5 D — IBL MVP (Ambient Diffuse Cube Lookup, 2026-07-23, ships)

### Context

§P5.5 A/B ship (2026-07-23) — `Light` POD + Point/Spot per-type math。§Skybox0 ship — equirect 2D backdrop,LightingPass `mix(skyColor, lit, coverage)` 当 unlit-area backdrop。

**本刀目标 (post-§P5.5 B, 2026-07-23)**: 接通 IBL MVP ── host 调 `Renderer::setSkySourceCube(cubeHandle)` 后,
- SkyboxPass::execute 的 CubeMap kind 走全屏三角 per-pixel lat/long→dir + `sample(skyCube, dir)` 出图
- LightingPass ambient term 替换 ── `sample(envCube, N) * albedo * ambientStrength` 替代 flat `vec3(0.1)` (当 cubeActive=1)
- host 不调 / cube handle invalid ⇒ 回落 pre-D byte-equivalent (flat `vec3(0.1)`)

**主路线决定 (文字版已拍)**:
1. **scope 只 ambient diffuse** ── 不掺 spec lobe / 不全 mip chain;radiance / prefilter / roughness-driven LOD = §P5.5 D-radiance 后续切
2. **SkyboxPass cube kind 接缝 ── 全屏三角 + per-pixel lat/long→dir** ── 不引 cube mesh,mirror equirect 的 cheap backdrop 风格,只换 sampler/FS
3. **host 上传路径 ── `Renderer::setSkySourceCube(TextureHandle)` API** ── 借 ptr 形态不可用 (cube handle 是 Resource 不是 borrowed ptr) ── 新 setter 转发到 SkyboxPass producer state (`_skyCubeTexture` field)

### 设计契约

| 维度 | 决策 |
|---|---|
| **数据源** | `ayt::render::SkySource` host POD ── `kind` (Equirect / CubeMap) + `equirect` (TextureHandle) + **`cubeMap`** (TextureHandle,D 替换原 `cubeReserve: uint6464_t` placeholder);`isActive()` ⇒ `kind==Equirect && hasEquirect() || kind==CubeMap && hasCubeMap()` |
| **host upload API** | `Renderer::setSkySourceCube(TextureHandle)` + `skySourceCube() const noexcept` getter;setter 内部也 forward 到 SkyboxPass producer state (cutsheet producer-state pattern ── mirror shadowFbo / lightingFbo / gbufferAlbedoRt) |
| **borrowed ptr / ctx field** | 0 新 PassExecContext field;cube handle 走 producer state (SkyboxPass 内部 `_skyCubeTexture`),host 通过 `ctx.skyboxPass->cubeTexture()` + `hasCubeActive(kind)` 读 |
| **Pipeline slot** | 不变 ── 复用 §Skybox0 Skybox slot |
| **几何** | 全屏三角复用 (no new mesh) |
| **view id** | 不变 ── Skybox 6 + Lighting 8 |
| **FS dual-path 决策** | **单 FS + uniform gating** ── `cubeActive` uniform (0/1 per-frame gate) + `ambientStrength` uniform (default 0.6);SkyboxPass skyKind uniform 同 pattern。省一半 program acquire overhead,mirror §Skybox0 single-FS 风格 |
| **Cache-key bump** | `skybox_v0_equirect_fullscreen` → `skybox_v1_equirect_or_cube_perpixel_dir`;`lighting_v21_p5p5b_point_spot_atten_cone` → `lighting_v22_p5p5d_ibl_ambient_cube` |

### 文件触碰 (7 文件,在 ≤ 8 红线内)

| # | 文件 | 改动 |
|---|---|---|
| 1 | `include/AYRenderScene.h` | `SkySource::cubeReserve: uint64_t` → `cubeMap: TextureHandle`;新增 `hasCubeMap()` getter;`isActive()` 加 cube 分支 |
| 2 | `include/AYRenderer.h` | 新增 `setSkySourceCube(TextureHandle)` + `skySourceCube() const noexcept` getter |
| 3 | `src/AYRenderer.cpp` | `Impl::skyCubeTexture` cache + setter 实现 (转发到 SkyboxPass via `findPass("Skybox")→setCubeTexture`) |
| 4 | `src/detail/SkyboxPass.h` | `extern kSkyboxCacheKeyCStr` + `_tSkyCube` / `_uSkyKind` binding IDs + `_skyCubeTexture` producer state + `setCubeTexture / cubeTexture / hasCubeTexture / hasCubeActive` accessors |
| 5 | `src/detail/SkyboxPass.cpp` | cache-key bump v0 → v1;Phoskia source 加 `texturecube skyCube` + `uniform float skyKind`;FS dual-kind branch with `mix(equirectColor, cubeColor, skyKind)`;execute body 加 cube sampler bind path + skyKind upload |
| 6 | `src/detail/LightingPass.h` | `_tEnvCube` / `_uCubeActive` / `_uAmbientStrength` binding IDs |
| 7 | `src/detail/LightingPass.cpp` | cache-key bump v21 → v22;Phoskia source 加 `texturecube envCube` + `uniform float cubeActive` + `uniform float ambientStrength`;FS ambient term `ambientFlat + ambientCube`;execute body 加 envCube sampler bind (from `ctx.skyboxPass->cubeTexture()`) + cubeActive / ambientStrength uniform uploads |

**触碰面 7 文件 ≤ 8 红线** ── 跟 §P5.5 B 持平。

### K1 关键 invariants

1. **cubeActive=0 default ⇒ flat `vec3(0.1)` ambient,byte-equivalent pre-D** ── Test_B7 `b7_ibl_cube_active_zero_default_pins_byte_equivalent` pin `ambientFlat` substring 仍出现;FS `ambientCube * cubeActive = 0`
2. **setSkySourceCube(invalid) ⇒ equirect path 完整保留** ── SkyboxPass producer state `_skyCubeTexture = TextureHandle{}`,`hasCubeActive()` 返 false,FS skyKind=0 ⇒ equirect branch;LightingPass cubeActive=0 ⇒ flat ambient
3. **setSkySourceCube(valid) + skySource equirect 同时有效 ⇒ cube path 赢 (硬规则)** ── cube handle valid + `SkySource::kind == CubeMap` ⇒ cubeActive=1 ⇒ FS ambient term 加 cube lookup;SkyboxPass skyKind=1 ⇒ FS `mix(equirect, cube, 1) = cube`,两 path 不各画一半
4. **SkySource POD 字段从 `cubeReserve: uint64_t` 升级 `cubeMap: TextureHandle`** ── ABI churn 仍守公共头不漏 bgfx:: (TextureHandle 已 ship 在 AYRenderTypes.h)
5. **LightingPass dual-FS 决策 ── 单 FS + uniform gating** ── `cubeActive` 是 uniform 不是 #ifdef;省一半 program acquire overhead,mirror §Skybox0 single-FS 风格
6. **SkyboxPass cache-key bump `v0 → v1_equirect_or_cube_perpixel_dir`** ── 强制 program re-acquire;`extern kSkyboxCacheKeyCStr` (mirror §P5.5 B Bug fix #3)
7. **§Skybox0 backdrop 行为保留** ── `mix(skyColor, lit, coverage)` 仍存;cubeColor 替换 equirectColor 但 backdrop 语义不变 (unlit area 仍填 sky)
8. **Forward host 0 行为变化** ── default `setSkySourceCube` 不调 = cubeActive=0 = pre-D;`makeDefault()` (Forward 5-slot) 不含 Skybox slot,Test_ForwardOpaque + Test_ShadowPass + Test_PostProcessPass + Test_TransparentPass 全保留
9. **触碰面 7 文件 ≤ 8** ── 不超红线
10. **scope 不掺 spec** ── ambient diffuse only,radiance / prefilter / roughness-driven LOD = §P5.5 D-radiance future cut

### Bug fix #3 mirror

`kSkyboxCacheKeyCStr` extern declared in `SkyboxPass.h:177-189` (mirrors §P5.5 B `kLightingCacheKeyCStr` extern). Test_Skybox0 的 `skybox_pass_cache_key_bump_v1_equirect_or_cube` 现在跟 live extern 比较,而不是 self-compare pre-D false-green ── drift 现在立即 fail。

### 不在 §P5.5 D scope (deferred 到 future)

| Item | Reason |
|---|---|
| Radiance / spec lobe / prefilter mip chain | §P5.5 D-radiance 后续切 (要 `textureCubeLod` + mip 生成 pipeline) |
| IBL diffuse convolution (irradiance map pre-bake) | D 刀只查 cube raw,radiance 后续 |
| Roughness-driven LOD | 跟 radiance 一并 |
| HDR cube (RGB16F) | D 刀只 RGBA8 (8-bit,no Float texture path);future cut 加 |
| PMREM (Pre-filtered Mipmap Radiance Environment Map) | future |
| Diffuse IBL convolution shader | 留 radiance 切 |
| host cube 数据上传 helper (`RenderResourceManager::createCubeTexture` + `BGFXAdapter::createCubeTexture`) | §P5.5 D-upload 单独 PR (本切只 ship `setSkySourceCube(TextureHandle)` API ── host 用外部 `bgfx::createTextureCube` manage 或等 upload helper PR) |
