# AYRenderer — 从当前工作树继续的执行计划

**状态：** Active（已复核，可直接当 coding brief）  
**日期：** 2026-07-20  
**复核：** 2026-07-20 对照 `AYRenderer` **独立 git 仓库** HEAD `638265b`  
  (`feat(renderer): backfill 4 Pass files to route every bgfx call through BGFXAdapter`)  
**前提：** 以仓库内真实代码为准，不以过期的 `design.md` 开头声明为准。  
**关联：** `design.md`（目标架构，需在 P0 同步）、`README.md`、`docs/phase0-rd02-plan.md`（已落地，仅作历史）。  
**本文件：** 若仍 untracked，编码前先 `git add docs/execution-plan.md` 进基线，避免 brief 丢在脏树上。

---

## 0. 怎么用这份文档

1. **一次只做一个 Phase**；每个 Phase 有明确退出门（测试 + 可验证行为）。
2. Phase 内 PR 尽量 **≤ ~8 个语义相关文件**；大 bang（Light + ShadowMap + 签名变更 + 默认挂 Pass）已证明会把 flaky 噪声和真实回归搅在一起。
3. 碰到 **AYRenderer_Test SIGSEGV / exit 139** 时，先走 §5 的隔离实验，**不要**用「撤掉整条 feature」代替根因；若必须 ship，只允许 §5.3 的 workaround 形态。
4. 改 Pass / FrameContext / RenderPass 签名前，先更新本文件对应 Phase 的「触碰面」列表。
5. **开工前拉取最新 `master` 后**：若 HEAD ≠ 文首复核 hash，先 diff 本计划 §1 / 附录 B，再动代码。

### 0.1 复核摘要（相对初版 plan 的修正）

| 点 | 初版 | 复核后 |
|----|------|--------|
| P2 场景 FBO API | 暗示新建 `createColorDepthFrameBuffer` | **复用**已有 `BGFXAdapter::createFrameBuffer(w,h,RGBA8,withDepth=true)` |
| Shadow「默认关闭」 | 易误解为 `setEnabled(false)` | 现状是 **根本未 `addPass`**；基类 `_enabled` 默认 **true** |
| `setShadowsEnabled` / `findPass` | 计划当可选手段 | **均不存在**，P3 要自建 |
| PostProcess 缺口 | 「常已画在 backbuffer」 | 更准：PP **绑自己的空 FBO 再采 attach0**，从未采场景色 |
| Post knobs / `resize` | 未写清 | **已有** `setPostProcess*` + `resize()`；缺的是 scene RT 策略 |
| ShadowPass 注释 | 「Light 导致 segfault」「50-unit frustum」 | **两处皆谎**；P0 必须改（identity only；segfault 见 §5） |
| Composite view | 笼统冲突 | **锁死：0=clear，1=scene，2=UI（`UIRenderBackend::kViewId`）**；PP 勿占 2 |

**判决：** 管线 / ABI / §5 约束与代码一致 → **可以从最新 git 按 §10 队列编码**。先 P0，再选 P1 或 P2；**禁止**跳过 §5 直接做 ShadowMap / 非 const Frame / 默认 enable Shadow。

---

## 1. 当前工作树真相（2026-07-20 / HEAD `638265b`）

### 1.1 产品默认管线（已接线）

`Renderer::Impl` 构造（`src/AYRenderer.cpp`）：

```
[0] ForwardOpaquePass
[1] TransparentPass
[2] PostProcessPass
[3] UIPass
```

`design.md` 里的完整目标序：

```
Shadow → GBuffer → Lighting → ForwardOpaque → Transparent → PostProcess → UI
```

其中 **Shadow / GBuffer / Lighting 均未进入默认管线**。ShadowPass 源码与单测存在，仅手搓 `RenderPipeline` 可挂。

### 1.2 子系统状态

| 子系统 | 状态 | 说明 |
|--------|------|------|
| BGFXAdapter + sticky Noop 生命周期 | Done + 债 | 进程级 refcount；同进程 Noop↔GPU 切换受限 |
| RenderResourceManager / AssetBridge / Skin upload | Done | RD-02/04 已落地 |
| ForwardOpaque（含 bone UBO） | Done | **不按 BlendMode 过滤** → Alpha 被 FO+Transparent **双画**（Transparent 有 `blendMode != Alpha` skip） |
| Transparent + `sortKey` | Partial | 只画 `BlendMode::Alpha`；相机 Z 自动填 sortKey 未做 |
| PostProcess | Partial | Phoskia + **自有** FBO + 公开 knobs（`setPostProcess*`）已在；**采样的是清空后的自有 FBO，不是场景色** |
| UIPass + UIRenderBackend + composite views | Done | view **0=clear / 1=scene / 2=UI**；头注释仍有「NOT YET DISPATCHED」谎言 |
| ShadowPass cut-1→F1' | Partial | depth FBO + **light-space ortho**（pass 本地矩阵）；**未 `addPass`**；无采样；无 Light / Frame shadow 槽 |
| GBuffer / Lighting | Missing | 仅设计 |
| DrawListBuilder / CommandQueue | Missing | 设计延后 |
| `design.md` / README / 多处 Pass 头注释 | Drift | 文档落后实现，是「乱」的主因之一 |

### 1.3 关键契约（不要在清理阶段偷偷改）

| 契约 | 现状 | 备注 |
|------|------|------|
| `RenderPass::execute` | 12 参，`const FrameContext&` | 保持 const，直到 §5 隔离实验通过 |
| `FrameContext` | view/proj/camera/light + time + post knobs | **无** ShadowMap 槽 |
| `RenderScene` | DrawItem 列表 | **无** Light struct / `addLight` |
| Pass → GPU | 允许 `#include <bgfx/bgfx.h>` | HEAD 已 backfill：draw 路径走 Adapter；新建 RT 用 `createFrameBuffer` / `createDepthOnlyFrameBuffer` |
| 公开头 | 无 bgfx | `Test_PublicHeaderSurface` 守门 |
| 已有、勿重复造 | — | `createFrameBuffer`、`createDepthOnlyFrameBuffer`、`setViewFrameBuffer`、`setPostProcess*`、`resize()`、`setMaterialBlendMode` |
| 尚不存在 | — | `setShadowsEnabled`、`findPass` / `insertPassBefore`、`PassExecContext`、`FrameContext::ShadowMap`、`RenderScene` Light |

---

## 2. 「乱」的根因（先认清，再动手）

按优先级：

1. **文档与代码双轨** — design 仍写「src 未创建 / R5 全延后」；头文件注释写「PostProcess deferred / UIPass 未 dispatch」；新人会按错误地图改。
2. **半成品叠半成品** — Shadow cut-1、PostProcess R5.1、Forward+Transparent 分流各自独立进化，**没有共享的帧 RT / 帧附件契约**。
3. **12 参 execute + 仅靠 ctor `addPass` 定序** — 扩展 ShadowMap / MRT 只能改签名或塞全局，易 big-bang。
4. **测试脆弱面与功能改动耦合** — Noop 多实例、`shaderc` 子进程、第二 `Renderer` 路径上的 flaky SIGSEGV；一次改 ABI 看不出是功能 bug 还是生命周期 rot。
5. **过期注释误导排查** — `ShadowPass.h` 仍写「Light struct … caused the prior segfault」与「fixed 50-unit-radius frustum」（实际 identity）；**未证实**归因见 §5。

清理原则：**先对齐地图与小契约 bug，再加能力；能力用 feature-flag / 默认关闭挂载，避免默认管线被 stub 污染。**

---

## 3. 产品决策锁（本计划假定）

此前已对齐的方向（若推翻，先改本节再改代码）：

| 决策 | 内容 |
|------|------|
| 双路径 | **ForwardPath**（现状演进）与 **DeferredPath**（GBuffer+Lighting）长期共存；Forward 作 fallback / 特殊物体 |
| Hybrid | 不透明可走 deferred；透明 / 特殊仍 forward |
| Shadow | **共享** ShadowPass；先于 GBuffer |
| 拒绝 | 「Lighting 之后再跑一遍完整 ForwardOpaque 画所有不透明」的扁平全跑序 |
| cut 序 | **先 Shadow 可用（含采样）→ 再 GBuffer+Lighting**；PostProcess 场景 RT 可与 Shadow 并行，但不阻塞 Shadow 采样 |

---

## 4. 总路线图（Phase 总览）

```
P0  地图对齐 + 小契约修复          ← 立刻做，低风险
P1  Pass 执行上下文收敛            ← 减乱，为 Shadow/RT 铺路
P2  场景离屏 RT + PostProcess 闭环 ← 画面正确性
P3  Shadow cut-2（默认关闭挂载）   ← 遵守 §5 隔离规则
P4  Forward 采阴影                 ← 前向阴影可用
P5  Deferred 最小闭环（可选）      ← GBuffer + Lighting
P6  合批 / 命令队列 / 文档收口     ← 性能与长期维护
```

建议节奏：**P0 → P1 →（P2 与 P3 可交错，但各 PR 隔离）→ P4 → 再决策是否进 P5。**

---

## 5. Segfault 约束（强制）

### 5.1 结论（截至 cut-1 ship）

- **根因未定位**；曾在 big-bang（Light + `FrameContext::ShadowMap` + `FrameContext&` 非 const + 默认 `addPass(Shadow)`）后稳定看到 exit 139。
- Crash site 常落在 `Test_RenderResources::textured_material_draw_one_frame` 的 **第二个 Renderer** → `createMaterialFromPhoskia` / **shaderc 子进程**路径。
- Bisect 表明：**仅撤 ShadowPass dispatch / 仅留 Shadow 新文件 + `createDepthOnlyFrameBuffer` / 甚至 stash 回「曾 264 PASS 的 HEAD」都可能复现** → 与「本次 Shadow 改动」相关性 **未确认**；存在 **pre-existing flaky / bgfx Noop 多 init 腐化** 成分。
- 已有缓解：`BGFXAdapter` 进程级 refcount + sticky Noop（`57908fd` 一类修复），**未证明修干净**。

### 5.2 已被证伪或未证实的猜测

| 猜测 | 状态 |
|------|------|
| ShadowPass::execute 本身 | Bisect 撤 dispatch 仍 crash → **不是唯一 culprit** |
| `const FrameContext&` → `FrameContext&` | 与其它改动绑在一起；**未单独证实** |
| `FrameContext::ShadowMap` ABI / DrawItem layout | 无 ShadowMap 的 bisect 仍 crash → **单独不足以解释** |
| `Light` 默认 member-init UB | enum class 0 是 well-defined → **弱** |
| shaderc pipe / Noop handle table 损坏 | **最像已知类问题**，需隔离实验 |

### 5.3 允许的 ship 形态（cut-1 已采用，cut-2 前仍遵守）

**禁止**在同一 PR 同时引入：

1. `RenderScene` Light struct（或等价多光源 API）
2. `FrameContext` ShadowMap 槽（fbo/depth/lightViewProj/…）
3. `RenderPass` / `executeAll` 签名改为非 const `FrameContext&`
4. 默认管线 `pipeline.addPass(ShadowPass)` 且默认 enabled

**允许：**

- ShadowPass 源码 + `createDepthOnlyFrameBuffer`
- 单测手搓 pipeline 挂 Shadow
- 默认管线 **不挂**，或挂了但 **`setEnabled(false)`** 且无 FrameContext 扩展
- identity light xform / 本地 `_shadowFbo`（不写回 FrameContext）

### 5.4 解锁 Light / ShadowMap / 非 const Frame 前必须做的隔离实验

每个实验 **单独 PR 或单独 stash 验证**，同一实验跑 **≥3 次** 全量 `AYRenderer_Test`（记录 flaky）：

| # | 实验 | 通过标准 |
|---|------|----------|
| E1 | 仅追加 `FrameContext` 尾部 POD 字段（无语义使用） | 不引入新稳定 crash |
| E2 | 仅追加 `RenderScene` Light 存储 API，Pass 不读 | 同上 |
| E3 | 仅改 `const FrameContext&` → `FrameContext&`（Pass 体仍不写） | 同上 |
| E4 | 默认 `addPass(Shadow)` + `setEnabled(false)` | 同上 |
| E5 | E1+E4 后 Shadow enabled，仍不写 FrameContext ShadowMap | 同上 |
| E6 | 在 E5 上把 shadow 句柄经 **旁路**（Pass 查询 API / Renderer getter）交给 Forward，**不**进 FrameContext | 验证「采样」是否必须进 Frame |

任一实验稳定挂掉 → 先加深 Noop/shaderc 生命周期（见 P0.5），再重跑，**不要**叠加下一个实验。

### 5.5 PR-F1（C' 路径）SIGSEGV — 2026-07-20 阻断记录

**目标路径（已确认会挂，禁止重开同一组合）：**
Light struct + FrameContext shadow 槽（`shadowFbo` / `lightViewProj` 等）+ 默认 `addPass(Shadow)` **enabled** + 真 light-space ortho；FO/Transparent **不**采样（原定 PR-F2）。

| 步 | 内容 | 结果 |
|----|------|------|
| 1 | F1 完整（5-pass + Light + FrameContext shadow + light ortho） | SIGSEGV @ `Test_RenderResources::textured_material_draw_one_frame` |
| 2 | Shadow `setEnabled(false)`（文件保留） | 仍 SIGSEGV（同位置） |
| 3 | 从 pipeline **完全移除** Shadow | 仍 SIGSEGV（同位置） |
| 4 | 回滚 FrameContext shadow 槽 + Light struct + `lastFrameShadowFbo` cache | **3/3** 稳定全绿（报告：447/447） |

**诊断（写入时点）：**
- SIGSEGV **不是** ShadowPass::execute 触发（步 3 仍 139）。
- **不是**「仅有 E1 已 ship 的 `shadowMapId` 尾部 POD」单独触发（E1 曾通过；步 4 回滚的是更大的 shadow 槽 + Light + cache）。
- 与 Shadow 是否挂管线无关 → 嫌疑集中在 **RenderScene Light / FrameContext 扩展 shadow 句柄矩阵 / Renderer 侧 lastFrameShadow 缓存** 的组合，或与 **pre-existing** `textured_material_draw_one_frame` flaky rot 叠加。
- 回滚到 matched master（报告基线 `b66deb8`）后干净。

**强制后续形态（PR-F1' / 本实现）：**
- ✅ `buildDirectionalShadowMatrices` + ShadowPass 本地 `_lightView/_lightProj/_lightViewProj` + 本地 `_shadowFbo`
- ✅ 仍用 `FrameContext::lightDirection`（已有字段）
- ❌ 不引入 `RenderScene` Light
- ❌ 不引入 `FrameContext::shadowFbo` / `lightViewProj`（E1 `shadowMapId` 保持只读占位，本 PR **不写**）
- ❌ 不引入 `lastFrameShadowFbo`
- ❌ 不默认 `addPass(Shadow)`（enabled 或 disabled 都先不做，直到单独 E4 3/3）

---

## 6. Phase 详单

### P0 — 地图对齐 + 小契约修复（1–2 PR）

**目标：** 让「乱」先变成「读得懂的半成品」，并修一个真实逻辑 bug。

| 项 | 动作 | 退出门 |
|----|------|--------|
| P0.1 | 重写 `design.md` 文首状态：R0–R4+Engine 已落地；默认 4 Pass；Shadow cut-1 旁路；GBuffer/Lighting 未做；公开头为 flat `include/*.h` | 文首与 §8/§13/§16 无自相矛盾 |
| P0.2 | `README.md` R5+ 行改为「PostProcess partial / Shadow stub unregistered / Deferred missing」 | 与代码一致 |
| P0.3 | 清过期注释：`UIPass.h`（`NOT YET DISPATCHED`）、`RenderPipeline.h`（仍写 3-pass / PostProcess deferred）、`AYRenderer.cpp` Impl「no-op slot」与 `render()` 漏提 PP 的注释、`AYRendererSubSystem` composite 注释、`ShadowPass.h`（删「Light 导致 segfault」+「50-unit frustum」，改链本文 §5） | grep 无上述谎言字符串 |
| P0.4 | `ForwardOpaquePass`：**跳过 `BlendMode::Alpha`**（Transparent 已画） | 单测：同 scene 下 Alpha 只在 Transparent 产生 draw；Opaque 计数下降符合预期 |
| P0.5 | 将 sticky Noop / 「同进程勿混 backend」写进 `design.md` 测试约定短节 | 后人不再当偶发神 bug |

**非目标：** 不改 Pass 签名；不挂 Shadow；不动 PostProcess 算法。

**触碰面：** `design.md`、`README.md`、若干 `.h` 注释、`ForwardOpaquePass.cpp`、对应 unittest。

---

### P1 — Pass 执行上下文收敛（1–2 PR）

**目标：** 消灭「再加一个 RT 就改 12 个文件签名」的主因，**不改变默认画面**。

| 项 | 动作 | 退出门 |
|----|------|--------|
| P1.1 | 引入 `detail::PassExecContext`（或扩 `FrameContext` 的 **只读视图**）：打包 `adapter/pool/scene/meshes/textures/materials/viewport/frame/viewId` | `RenderPass::execute(PassExecContext&)`；行为 golden 不变 |
| P1.2 | `RenderPipeline::executeAll` 同步改；所有 Pass 一次迁完 | 全量单测 PASS |
| P1.3 | （可选）`RenderPipeline::findPass(name)` / `insertPassBefore(name, …)` | Shadow 可默认关闭插入，无需改 Impl 顺序魔术 |

**约束：** `frame` 字段仍为 **const 引用**（满足 §5.3）。可变的仅 `materials`（现有 BindingId 懒缓存）。

**非目标：** 不引入 ShadowMap 字段；不做 deferred。

---

### P2 — 场景离屏 RT + PostProcess 输入闭环（2–3 PR）

**目标：** PostProcess 采到的是「场景色」，UI 仍在其后画到 backbuffer。

**ViewId 锁（勿发明 N/N+1/N+2 抢 UI 槽）：**

| 模式 | view 0 | view 1 | view 2 |
|------|--------|--------|--------|
| Composite（Editor） | 全窗 clear | 3D scene（可绑 scene FBO） | UI（`UIRenderBackend::kViewId = 2`） |
| 非 composite | scene→FBO（或 PP 用 1 写回 backbuffer） | Post→backbuffer（若需要独立 view） | 仅当有 UI backend 时用 2 |

**今日缺口（复核确认）：** FO/Transparent 画 **backbuffer**；`PostProcessPass` `setViewFrameBuffer` 绑 **自己的空 FBO**，再 `getFboAttachment(_fbo,0)` 采样 —— 不是场景色。

| 项 | 动作 | 退出门 |
|----|------|--------|
| P2.1 | **复用** `BGFXAdapter::createFrameBuffer(w,h,RGBA8,/*withDepth=*/true)` 做 scene RT；**不要**新建 `createColorDepthFrameBuffer`（除非纯别名） | Noop→invalid；resize 后下一帧 `ensure*` 重建 |
| P2.2 | Renderer：FO+Transparent 绑 scene FBO；PostProcess **改采 scene attach0**（可销毁或缩小 PP 自有「假输入」FBO 职责） | tonemap/bloom≠identity 时真 GPU 有可测差 |
| P2.3 | Noop：整条仍 0-cost / 不崩；**勿占 view 2** | `Test_PostProcess*` / `Test_CaptureScreenshot` / `Test_UIPass_AI1` 不回归 |
| P2.4 | 文档：无 scene RT 或 knobs 全默认时 PP = identity / early-out | README 一段 |

**风险：** composite 路径已占用 0/1/2 — P2 与 `beginCompositeFrame` **同 PR 或紧随 PR** 对齐，禁止 Post 占用 view 2。

**非目标：** 完整 bloom downsample chain 可仍 stub；先保证采样源正确。

**已具备、勿重做：** `setPostProcessBloomStrength/Exposure/TonemapMode`、`resize()` 公开 API。

---

### P3 — Shadow cut-2（默认关闭，遵守 §5）（2–4 PR）

**目标：** 有真实 light-space depth；仍可不进默认采样。

| 项 | 动作 | 退出门 |
|----|------|--------|
| P3.0 | 跑完 §5.4 E1–E4（记录结果进本文件附录） | 有书面结果表 |
| P3.1 | Light 方向来源：优先 **复用** `FrameContext::lightDirection`；ortho/`lookAt` 建 light VP；矩阵落在 **ShadowPass 成员**（见 §5.5 PR-F1'） | 单测钉 matrix 非 identity + 随方向变 |
| P3.2 | 场景 AABB / 固定半径 frustum — **F1' 已选固定半径 50** | 文档与 `kDefaultFrustumRadius` 一致 |
| P3.3 | **暂缓**默认 `addPass(Shadow)`（含 disabled）— 等 §5.4 E4 单独 3/3；禁止与 Light/Frame shadow 槽同 PR | 默认管线仍 4-pass |
| P3.4 | **不要**把 shadow 句柄/矩阵写入 `FrameContext`；用 ShadowPass getter（`lightViewProj` / `shadowFbo`） | Forward 仍不读（F2） |

**显式推迟：** cascade、PCF、点光阴影、Phoskia `shadow_caster` 变体、alpha-clip caster。

---

### P4 — Forward 采阴影（1–2 PR）

**依赖：** P3 + §5.4 E6（或 E1 证明 FrameContext 槽安全）。

| 项 | 动作 | 退出门 |
|----|------|--------|
| P4.1 | SimpleLit / 现有 forward 材质增加可选 `texture2dshadow` 或深度比较采样 | 有灯方向时地面有阴影；关 ShadowPass 无采样 |
| P4.2 | bias 常量 + 文档 | 无严重 acne 的 demo 截图 |
| P4.3 | `setShadowsEnabled(true)` 默认仍 false | 引擎 / Editor 显式打开 |

---

### P5 — Deferred 最小闭环（可选大项）

**仅当** P4 稳定且产品确认需要多光 / 复杂不透明 shading。

| 项 | 动作 | 退出门 |
|----|------|--------|
| P5.1 | `GBufferPass` MRT（albedo/normal/motion 等最小集） | Noop 短道路径；真 GPU 可视化 GBuffer |
| P5.2 | `LightingPass` 读 GBuffer + 共享 Shadow | 一盏方向光 parity ≈ Forward |
| P5.3 | `RenderPath` 枚举：`Forward` / `Deferred`；透明始终走 TransparentPass | 切换路径不崩；Forward fallback 保留 |
| P5.4 | **禁止** Lighting 后再用 ForwardOpaque 画全部不透明 | code review 检查清单 |

---

### P6 — 收口（可与 P4/P5 并行零碎做）

| 项 | 动作 |
|----|------|
| P6.1 | Transparent：可选自动 `sortKey = -cameraZ`（Host 可覆盖） |
| P6.2 | DrawListBuilder 合批（同 shader/material） |
| P6.3 | PostProcess 真 bloom 链或删除假参数 |
| P6.4 | `docs/phase0-rd02-plan.md` 标 **Superseded / Landed** |
| P6.5 | Pass 内残留裸 `bgfx::` 创建/destroy 收到 Adapter（与 design § 硬规则一致） |

---

## 7. 建议 PR 切片（可直接当 ticket）

| PR | 标题草案 | Phase |
|----|----------|-------|
| PR-A | docs: sync design/README with 4-pass reality + segfault constraints | P0.1–0.3, 0.5 |
| PR-B | fix: ForwardOpaque skips BlendMode::Alpha | P0.4 |
| PR-C | refactor: PassExecContext collapses RenderPass::execute arity | P1 |
| PR-D | feat: scene `createFrameBuffer` feeds PostProcess（勿占 view 2） | P2 |
| PR-E | experiment: FrameContext POD tail **或** default-pipe Shadow+disabled（一次一个） | §5.4 E1 / E4 |
| PR-F | ~~C' Light+ShadowMap+enabled~~ **阻断** → **PR-F1'** light-space on ShadowPass only（§5.5） | P3 / §5.5 |
| PR-F2 | feat: forward shadow sampling via pass getters（无 FrameContext 槽） | P4 |
| PR-H | feat: GBuffer + Lighting path switch | P5 |

---

## 8. 测试策略（每个 Phase 通用）

1. **默认：** `AYRenderer_Test` 全绿；无新的稳定 139。
2. **Flaky 嫌疑：** 同一 commit 连续跑 3 次；失败则贴 §5，不合并「带赌」的 ABI 变更。
3. **shaderc：** 无 `AY_SHADER_SHADERC_HINT` 的 SKIP 保持；新增编译路径测试必须 SKIP-safe。
4. **真 GPU：** Shadow / PostProcess / scene RT 至少各留一个 demo 或截图路径（Noop 测不到深度内容）。
5. **回归锚点：** `Test_LightingCamera`、`Test_RenderResources`（多 Renderer）、`Test_PostProcess*`、`Test_ShadowPass`、`Test_UIPass_AI1`。

---

## 9. 明确不做（本计划窗口内）

- 把 Command Queue / 多线程 submit 当成 Shadow 的前置。
- 为「看起来整齐」重写整个 `Renderer::Impl`。
- 在根因未解时恢复 big-bang（Light + ShadowMap + 非 const Frame + 默认 enable Shadow）。
- 删除 Forward 路径「只留 Deferred」。
- 无必要的公开头目录搬迁（`include/AYRenderer/...`）——与 flat include 现状不一致，单独立项。

---

## 10. 立刻下一步（从最新 git 开写的默认队列）

工作目录：`D:\Projects\AYRuntime\AYRenderer`（**独立仓库**，不是 `D:\Projects` 根）。

```text
cd AYRuntime/AYRenderer
git status          # 确认干净或只含本 plan
git pull            # 若有 remote
# 建议：先 commit 本 plan，再开 feature 分支
git checkout -b fix/renderer-p0-docs-alpha
```

编码顺序：

1. **PR-A** — `design.md` 文首去掉「src 未创建」；README R5+ 行改成 partial；清 §P0.3 注释清单；design 短链到 `docs/execution-plan.md` §5  
2. **PR-B** — `ForwardOpaquePass::execute`：`if (material.blendMode == BlendMode::Alpha) continue;` + 单测钉「Alpha 只在 Transparent 计数」  
3. **二选一**  
   - 要减后续改签名成本 → **PR-C**（`PassExecContext`，`frame` 仍 const）  
   - 要先修画面 → **PR-D**（scene `createFrameBuffer` → PP 采样；守 view 2）  
4. **任何** Shadow enable / Light / ShadowMap / 非 const Frame **之前** → §5.4 **E1 或 E4 单独跑 ≥3 次**，填附录 A  

**不要**从 Shadow cut-2 或 Deferred 起手。

---

## 附录 A — Segfault 隔离实验结果 + PR 落地记录

| 实验 / PR | 日期 | 跑次 | PASS/FAIL | 备注 |
|------|------|------|-----------|------|
| **E1** FrameContext 尾部 POD | 2026-07-20 | **4/4 PASS** (428/428) | tail `uint32_t shadowMapId = 0;`,无语义使用;commit `938c56d` (branch `exp/renderer-framecontext-tail-pod`);4 跑次出于 §5.4 baseline 协议额外加固一次;解锁 E4 / P3 路径 |
| **P2 / PR-D** Scene RT → PP 闭环 | 2026-07-20 | **3/3 PASS** (447/447) | 见既有记录；`sceneFbo` on PassExecContext |
| **PR-F1 C'** Light+Frame shadow槽+enabled Shadow | 2026-07-20 | **FAIL 139** | 见 §5.5；步1–3 均 SIGSEGV；步4 回滚后 3/3 PASS；基线 `b66deb8` |
| **PR-F1'** light-space on ShadowPass only | 2026-07-21 | **3/3 PASS** (465/465) | `ShadowLightMatrix.h/.cpp` 用 bx 构 view/proj(SAME `homogeneousDepth` 约定同 `setMainCameraLookAtPerspective`);`ShadowPass::_lightView/_lightProj/_lightViewProj` 内部缓存 + getter(`lightView()/lightProj()/lightViewProj()/shadowFbo()`);**不**增 `RenderScene Light`、**不**加 `FrameContext shadow 槽`(改设 `AY_F1_DIAG_LIGHT / AY_F1_DIAG_FRAME_SHADOW / AY_F1_DIAG_DEFAULT_SHADOW` CMake feature flag,默认全 OFF ⇒ master ABI 与 `b66deb8` 一致);**不**默认挂 Shadow;新文件 `include/AYF1DiagFlags.h` + `unittest/Test_F1_LayoutDiag.cpp` 锁住 ABI 一致(测试在三个 flag 全 OFF 时打印 `sizeof(RenderScene)` / `sizeof(FrameContext)` 防混编复发);**附带真修**:`FrameContext.h` 不再 `#include <bgfx/bgfx.h>`(与 `MemorySystem::instance` 在 `AYRendererSubSystem.cpp` 撞名),shadow 槽只存 `uint16_t shadowFboIdx` 而非 `bgfx::FrameBufferHandle`;18 新测试(447 → 465);诊断面板见 `docs/f1-sigsegv-repro.md` |
| E2 Light 存储 API | | | | **默认 OFF**;开 `AY_F1_DIAG_LIGHT=ON` 时启用,确保 Clean 全编 |
| E3 非 const FrameContext& | | | | 未跑 |
| E4 默认挂 Shadow disabled | | | | **默认 OFF**;开 `AY_F1_DIAG_DEFAULT_SHADOW=ON` 时启用,需 §5.4 干净树重跑 ≥3 次 |
| E5 Shadow enabled 无 Frame 槽 | | | | 未跑(在 `AY_F1_DIAG_LIGHT=OFF + AY_F1_DIAG_FRAME_SHADOW=OFF` 下应是基线,需独立验证) |
| E6 旁路 getter 采样 | | | | F1' 已提供 getter(lightView/lightProj/lightViewProj/shadowFbo),**采样**属 PR-F2 |

## 附录 B — 关键默认管线索引

| Index | Pass | 默认 |
|-------|------|------|
| 0 | ForwardOpaque | on（**含 Alpha，待 P0.4 修**） |
| 1 | Transparent | on（仅 `BlendMode::Alpha`） |
| 2 | PostProcess | on（P2：采 `ctx.sceneFbo` attach0；Noop 仍 early-out） |
| 3 | UI | on（无 backend 则空） |
| — | Shadow | 代码存在，**未 `addPass`** |
| — | GBuffer / Lighting | 不存在 |

## 附录 C — 关键 bgfx view 分配

| View | 用途 |
|------|------|
| 0 | 非 composite：主场景；composite：`beginCompositeFrame` 全窗 clear |
| 1 | composite：3D scene（`CLEAR_NONE`） |
| 2 | UI（`UIRenderBackend::kViewId`）— **P2/P3 禁止挪作 post/shadow 输出** |

Shadow 深度 FBO 使用 **独立 viewId**（由 ShadowPass / Adapter 分配，避开 0–2）或仅在非 composite 测试管线中使用；接到默认管线前写进本附录。

## 附录 D — 关键关键文件（编码入口）

| 用途 | 路径 |
|------|------|
| 默认 `addPass` 序 | `src/AYRenderer.cpp`（`Impl` ctor ~L87–98） |
| Pass 基类签名 | `src/detail/RenderPass.h` |
| Frame 数据 | `src/detail/FrameContext.h` |
| FBO API | `src/detail/BGFXAdapter.h`（`createFrameBuffer` / `createDepthOnlyFrameBuffer`） |
| Alpha 双画 | `src/detail/ForwardOpaquePass.cpp` + `TransparentPass.cpp` ~L101 |
| PP 错误输入 | `src/detail/PostProcessPass.cpp` |
| Shadow 矩阵 | `src/detail/ShadowLightMatrix.{h,cpp}`、`ShadowPass.{h,cpp}` |
| Composite views | `include/AYUIRenderBackend.h`、`include/AYRenderer.h` `beginCompositeFrame` |
| 公开 blend/post | `include/AYRenderer.h` `setMaterialBlendMode` / `setPostProcess*` |
