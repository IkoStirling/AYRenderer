# Deferred Pass（GBuffer + Lighting）使用说明（B0 ── 现场重置，预设）

> **状态**：B0 ── 现场重置（docs only,0 代码,2026-07-22）。  
> 配套 [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) 看 B1–B6 红线 / 切片 / 复用 API 表。  
> 配套 [`execution-plan.md`](execution-plan.md) §P5 / §5.3 看优先级 / 红线 / 隔离实验。  
> 下一刀候选：B1（RenderPath enum）。在 B1 ship 前，本 Pass **不挂默认管线**。

---

## 0. 目的

让 Editor / demo 在多盏方向光（及更复杂的非方向光）场景下，**Forward O(n) per light + 不透明 per-pixel 重算** 的瓶颈不再限制内容创作。Deferred path 把不透明渲染收成两步：

1. **GBuffer** ── 一次性把所有不透明物体写出 albedo / normal / motion / depth 4-slot MRT。
2. **Lighting** ── 全屏三角形一次 sample 这 4 张 RT，按光源逐盏求和，写到 deferred color buffer。

Forward path 保留作为默认、皮肤 / 半透明 / 特殊材质 fallback ── 与 `pass-lessons-from-deferred.md` §3 「双路径共存」决策一致。

---

## 1. 快速启用（B6 ship 后形态，B0 仅是文档预写）

```cpp
// §P5 B6 — deferred opt-in factory,默认 Forward 不变。
renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
```

Editor Play 默认仍走 `makeDefault()`（Forward）── host 显式 opt-in 即切 path。stamp 见 stderr（**B4/B5 ship 后写入**，当前 `v13-phase7-vec4-abi` 是 shadow/forward 阶段）。

---

## 2. 场景数据

| 物体 | `DrawItem` 要求 | 备注 |
|------|----------------|------|
| 立方体（不透明 caster） | 走 GBuffer RT0–2 + depth | **必须** `BlendMode::Opaque`（BlendMode::Alpha 仍走 Transparent） |
| 半透明物体 | `BlendMode::Alpha` | GBuffer **不**接收 Alpha；Transparent 走场景合成（在 Lighting 输出后） |
| 皮肤 / 特殊材质 | `BlendMode::Opaque` 标 `forwardOnly=true`（B7+ 待开） | Deferred path 时仍走小段 Forward 子集（v1 不实现,留 Round 2） |
| 地面（仅接收 lighting） | 同 cube | GBuffer 仍写（receiver = geometry blocker）,只是 shader 不重读它 |

B6 ship 前：Deferred path 暂未对外公开，host 不应 `configurePipeline(makeDeferred())`。

---

## 3. Material / Pass 绑定契约

GBuffer / Lighting 不通过 `DrawItem::shadowFlags` 这类现有字段；它们通过 **RenderPath 隐式选择**：

```
GBuffer = 收所有 BlendMode::Opaque 的 DrawItem
Lighting = 全屏三角形 + sample GBuffer attachments(借用句柄)
Transparent = 仍按 BlendMode::Alpha,独立 pass
PostProcess = 在 Deferred path 改采 ctx.gbufferPass->lightingOutputFbo()
```

公开 API 锁定（**[B6]** 才外露, B0 仅声明）：
- `RenderPipelineDesc::makeDeferred()` ── 默认就是 Forward 5-pass；Deferred 是 opt-in 新 factory。
- `RenderPath pipelineDesc().path` ── 取当前 path。
- `bool pipelineDesc().isDeferred() const noexcept` ── host 编程用。

**禁止**（§5.3 红线）：
- ❌ 公开 API 加 `setDeferredEnabled(bool)` setter ── host 应走 `configurePipeline(makeDeferred())`，不上 setter 跟 Shadow E5 教训一致。
- ❌ 公开 API 加 `setLightPosition(...)` 或 Light struct ── 多光数据走 ctx.lights 借用指针（B7+），目前 1 盏方向光仍走 `FrameContext::lightDirection`（B5）。
- ❌ 公开头文件加 `<bgfx/bgfx.h>` ── `Test_PublicHeaderSurface` 已守门，所有 GPU 类型封装在 `detail::`。

---

## 4. View 分配

### 4.1 真实现网（2026-07-22 E5 ship 后 baseline，必对）

代码锁定的 view 分配 ── 在建新 view 前必须先把现网钉住，否则 B1/B3 直接撞：

| view id | 用途 | 出处 |
|---------|------|------|
| **0** | full-window clear | `Renderer::Impl::compositeSceneViewId` 重置；`beginCompositeFrame` 全窗 clear |
| **1** | Shadow caster（光照 depth FBO 写入）| `ShadowPass::kShadowViewId = 1`（`ShadowPass.h:33`）|
| **2** | Shadow resolve blit（color RT → sampleable tex）| `ShadowPass::kShadowResolveViewId = 2`（`ShadowPass.h:34`）|
| **3** | ForwardOpaque（FO 用 `ctx.viewId`，Renderer 推 3）| `AYRenderer.cpp:380,384` |
| **4** | Transparent（独立 view，原与 FO 共享 → 拆）| `TransparentPass::kTransparentViewId = 4`（`TransparentPass.h:37`）|
| **5** | PostProcess blit-to-backbuffer（独立 view，绝不可与 FO/Trans 共享）| `PostProcessPass::kBlitViewId = 5`（`PostProcessPass.h:68`）|
| **6** | UI chrome | `UIRenderBackend::kViewId = 6`（`AYUIRenderBackend.h:40`）|

**关键约束**：
- **view 2 已被 Shadow resolve blit 占用** ── B0 初稿曾错写 `kGBufferViewId=2`，**直接撞 Shadow** ── 现已删除。
- **view 1/2/3/4/5/6 全部被 Forward path 钉死** ── Deferred 必须 **新加 view id**，不能复用。
- PostProcess **绝不**占 view 2/3/4（避免破坏 FO 的 depth test + scene FBO blit 路径）。
- UIPass 始终在 view 6（`UIRenderBackend` 内部硬绑） ── Deferred 也复用 view 6。

### 4.2 Deferred path view 分配（**B3 切换时钉死**）

每个新 Pass 必须有独立 viewId。**view id 不能复用 Forward 任何槽位**：

```
GBufferPass::kGBufferViewId    = 7   (B4 引入 — 避开现网 1/2/3/4/5/6 全占用)
LightingPass::kLightingViewId = 8   (B5 引入 — 避开现网)
ForwardOpaque / Transparent / PostProcess / UIPass / ShadowPass
   view id 在 Deferred path 仍复用 (§ 4.1 Forward 真值)
   ⇒ Forward 与 Deferred 共享 0–6；只新增 7/8
```

| Path | view 0 | view 1 | view 2 | view 3 | view 4 | view 5 | view 6 | view 7 (新 B4) | view 8 (新 B5) |
|------|--------|--------|--------|--------|--------|--------|--------|----------------|----------------|
| **Forward（默认,现网）** | full clear | Shadow caster | Shadow resolve | FO | Transparent | PP blit | UI chrome | ── | ── |
| **Deferred（opt-in B3+）** | full clear | Shadow caster | Shadow resolve | (FO skip) | Transparent | PP blit | UI chrome | **GBuffer MRT** | **Lighting** |

### 4.3 PostProcess source-FBO 选择优先级（B6 锁定）

PP 永远跑在 view 5 ── 但 source-FBO **两 path 不同**：

- **Forward path**：`ctx.sceneFbo`（Renderer 拥有的 FO+Trans 写过的 color+depth FBO，**已 ship**）─ 优先级 1；fallback 到 PP 自有 FBO（Noop test path）。
- **Deferred path**：`ctx.gbufferPass->lightingOutputFbo()`（GBufferPass 私有 LightingPass 输出 FBO，**B4 引入**）─ 优先级 1；fallback 到 `ctx.sceneFbo`（safe default，但视觉上 Alpha 会缺光）。

**约束**：
- B6 必须改 `PostProcessPass::execute` 的 source-FBO 选择优先级 ── 不能改 `PostProcessPass::kBlitViewId = 5`。
- PP 错用 view 7/8 → 抢 GBuffer/Lighting 槽 ⇒ 全黑或全亮 ⇒ 必崩。
- Deferred path 下 FO 完全 skip（Pipeline 显式不挂 `RenderPassSlot::ForwardOpaque`）；View 3 仍被 Renderer 拥有，但 **无 draw**。

### 4.4 公开 API 限定（host 视角）

- `RenderPath pipelineDesc().path` ── host 编程枚举。
- `RenderPipelineDesc::makeDeferred()` factory 隐式设定 path=Deferred + 包含 GBuffer/Lighting slot。
- **绝不**暴露 `set*ViewId` setter ── viewId 是 pass 内部 constexpr，不让 host 乱碰。
- **绝不**改 `UIRenderBackend::kViewId = 6` ── 后端硬绑。

---

## 5. 环境变量（预设,待 B6 ship 后写入）

| 变量 | 作用 | B 触碰 |
|------|------|--------|
| `AY_DEFERRED_LOG=0..4` | 诊断级别 L0–L4 | B6 |
| `AY_DEFERRED_DEBUG=1` | Lit 法线可视化 | B5 |
| `AY_DEFERRED_USE_SC=1` | 强制 hand `.sc` 金标（默认走 Phoskia） | B5 |
| `AY_DEFERRED_MRT_FALLBACK=1` | MRT 不能 attach 时退化单 RT | B4 |

当前 **无**：B0–B3 还在 plumbing 阶段，环境变量在 B6 前无意义。

---

## 6. 验收清单（Editor Play ── B6 ship 后写入）

1. stderr 含 `[Deferred]` stamp + `path=Deferred`；默认 `via Phoskia`；
2. 裸跑观感与 `configurePipeline(makeDefault())` forward 路径一张图精确比对：
   - 一盏方向光下完全一致；
   - 二盏方向光下 forward 跟 deferred 都对，但 deferred 单 pass 算两盏 = 像素级常量；
3. `textured_material_draw_one_frame` 3 跑稳（§5.5 历史 flaky 站点严守）；
4. `AYRenderer_Test` GBuffer / Lighting 相关 case 全绿；
5. 公开头无新增 `bgfx::*` 类型（`Test_PublicHeaderSurface` pass）。

---

## 7. 架构速查（B6 ship 后形态,B0 预写）

```
GBufferPass
  ├─ GBufferResources   RGBA8×3 MRT + D24S8 → ensure / destroy / resize
  ├─ GBufferCaster      draws all Opaque items, no Blending (writes MRT)
  └─ GBufferDiagnostics L0–L4 + GBufferFrameStats

LightingPass
  ├─ FullscreenTriangleLayout (shared with PostProcess)
  ├─ LightingMain        sample GBuffer attachments + Shadow reuse
  └─ LightingDiagnostics

Shadow (shared)
  └─ unchanged — LightingPass 通过 ctx.shadowPass 借用句柄采 sample
```

**PassExecContext** 新增 `const GBufferPass* gbufferPass = nullptr;`（**B2**,镜像 `shadowPass`）。
**PassExecContext** `sceneFbo` 在 Deferred path 时由 PP 改采 `ctx.gbufferPass->lightingOutputFbo()`，但 `sceneFbo` 字段**不重定义**。这跟 ShadowPass 把 shadow FBO 留在私有、不进 ctx 的铁律一致（`PassExecContext.h:53-55` 注释）。

---

## 8. 相关头文件（预设, B1–B6 ship 时创建）

| Header | 角色 | B 触碰 |
|--------|------|--------|
| `AYGBufferSettings.h` | mrt 尺寸 / format / build stamp | B4 |
| `AYGBufferShaderSources.h` | caster phoskia | B4 |
| `AYGBufferReceiverContract.h` | lighting 绑定契约 | B5 |
| `AYGBufferDiagnostics.h` | 日志级别 + frame stats | B4 |
| `AYGBufferConfig.h` | 伞头 | B6 |
| `AYLightingShaderSources.h` | lighting phoskia | B5 |
| `AYLightingReceiverContract.h` | lighting fragment 绑定契约 | B5 |

---

## 9. 已知限制 / 后续

- **只支持 1 盏方向光**（B5 ship 形态）。多光走 B7+ `ctx.lights` 借用指针，**永不进 FrameContext 永不进 RenderScene Light struct**（§5.3 红线）。
- **特殊材质（皮肤 / 半透明 / 自定义 blend）**仍在 Forward path：B6 不把它们切到 Deferred 子集，Round 2 开窗。
- **Noop backend**：B2/B3/B4/B5 全保持 Noop 0-draw；GBuffer/Lighting 真视觉仅真 GPU 可见。
- **PassExecContext ABI**：每刀加 0–1 个借用指针字段，sizeof 走 `Test_F1_LayoutDiag` 守门（ABI 一致性，§5 教训）。
- **默认管线 Forward 不变**：Deferred 是 opt-in，B6 后 host 可 `configurePipeline(makeDeferred())`。
- **文档先行**：本 Pass 文档（`deferred-pass.md`）会比代码早 ship，旨在让后续 B1–B6 编码有 cutsheet（avoid Shadow F1 翻车 4 commit bisect）。

---

## 10. 红线回顾（必读）

| 红线 | 违反后 |
|------|--------|
| 同时引入 `RenderScene::Light` + `FrameContext::GBufferFbo` + 默认 enabled + 多光 | **SIGSEGV at `textured_material_draw_one_frame`**（§5.5 F1 历史） |
| `FrameContext` 加 GBuffer 槽 | ABI 漂移，撞 EventBus `_Orphan_all`（§5.4 E1） |
| `RenderPass::execute` 改非 const `FrameContext&` | §5.4 E3 未跑，单独 stash 验证 |
| 默认 Forward 改成 Deferred | §5.3 红线 #4 ── host opt-in |

详见 [`execution-plan.md`](execution-plan.md) §5.3 / §5.4。
