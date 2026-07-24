# FrameGraph MVP Cutsheet（中期）

> 与 [`short-term-plan.md`](short-term-plan.md)、[`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`post-process.md`](post-process.md) 配套。  
> 日期：2026-07-23 · **现在只写契约，不写实现**。短期计划禁止开工本文件范围外的代码。

## 1. 要解决什么

当前管线靠显式 `RenderPipelineDesc` 槽位 + `PassExecContext` 借用指针。  
这对「拓扑固定、十个以内 Pass」够用；对 **半分辨率临时 RT / Ping-pong / 效果开关裁剪** 会复制粘贴爆炸。

FrameGraph MVP 只解决：

- 命名资源（谁写、谁读）
- 按开关裁掉未用 Pass / 未分配 RT
- Transient 同帧复用（简单 alias）

**不**解决：跨帧自动 history、多队列、替换整个 `RenderPass` 体系、自动 barrier 优化。

## 2. 范围（红线）

### 2.1 先盖这段（唯一允许迁入 FG 的区间）

```
[LightingOutput / Transparent 之后] → Bloom* / 其它 PP 效果* → Final PostProcess → UI
```

| 在范围内 | 不在范围内（禁止第一刀迁入） |
|----------|------------------------------|
| Bloom downsample / blur / upsample | ShadowPass |
| 将来的 SSAO / DOF / ColorGrade（若独立 Pass） | GBufferPass / LightingPass |
| Final tonemap+gamma（可仍叫 PostProcessPass） | SkyboxPass |
| | TransparentPass |
| | UIPass |

稳定的 Deferred 前半段继续用手写槽位；FG 只吃「Lighting 之后的图像处理链」。

### 2.2 与现有 API 共存

- 宿主仍调用 `configurePipeline(makeDeferred())`。
- FG 是 **Renderer 内部** 在 `Transparent` 与 `UI` 之间的子调度器，不是 Editor 新管线 API。
- 现有 `setPostProcessGamma` / `TonemapMode` / `Exposure` 继续驱动 **Final** 节点。

## 3. 最小对象模型（实现时照此切）

```text
FgResourceId   — 稳定字符串或枚举（SceneColor, BloomMip0, …）
FgTextureDesc  — format / scale(full|half|quarter) / transient?
FgPassDesc     — name, reads[], writes[], enabled, execute callback 或 RenderPass*
FrameGraph     — addPass / importExternal / compile / execute
```

### 3.1 导入的外部资源（不由 FG 创建）

| 名 | 来源 | 说明 |
|----|------|------|
| `SceneColor` | LightingOutput（或 Forward sceneFbo） | PP 链输入 |
| `SceneDepth`（可选） | GBuffer depth / scene depth | SSAO/DOF 以后再用；MVP 可不 import |

### 3.2 FG 自建 transient（MVP 示例）

| 名 | 尺寸 | 用途 |
|----|------|------|
| `BloomBright` | half | 阈值提取 |
| `BloomBlurA` / `BloomBlurB` | half | ping-pong |
| `PpComposite`（可选） | full | 若 Final 需要离屏再 blit；也可 Final 直接写 backbuffer |

### 3.3 Final 节点契约

- 读：`SceneColor` +（若 Bloom 开）`BloomBlur*`  
- 写：backbuffer / composite 目标（与今日 `PostProcessPass` blit 目标一致）  
- Shader：现有 ACES + gamma；Bloom 用 `bloomStrength.x` 或独立 sampler 合成  

## 4. Compile 规则（必须简单）

1. `enabled == false` 的 Pass **不分配** 其私有 write 资源（若无其它 Pass 读）。  
2. 被裁掉的 Pass 的下游若仍需要输入：用 **边旁路**（Bloom off ⇒ Final 只读 SceneColor）。  
3. 同尺寸、生命周期不重叠的 transient 允许 alias（MVP 可先不做 alias，只做「按需创建」）。  
4. viewId：继续由 Pass 内约定递增；FG 不发明第二套 view 分配器（避免与 Shadow/GBuffer 冲突）。  

## 5. 验收（中期开工时用）

1. Bloom off：与今日 Final-only 字节级观感一致（或文档声明的可接受差）。  
2. Bloom on：日志出现 FG compile 摘要（pass 数、RT 数）；无泄漏的每帧 create。  
3. 关 Bloom 后 RT 创建次数下降（或 alias 命中）。  
4. Deferred 健康日志清单仍全部满足（见 [`deferred-acceptance.md`](deferred-acceptance.md)）。  

## 6. 明确不做（防范围蠕变）

- 整帧 Shadow→UI 进图  
- 自动推导 Pass 顺序（顺序仍由注册顺序 / 显式边声明）  
- TAA history / SSR（需要跨帧 import，属长期）  
- 替换 `PassExecContext` 借用指针模型  
- Editor 暴露「FrameGraph 编辑器」UI  

## 7. 何时从「短期」升级到本 cutsheet 实现

满足 **任意两条** 再开 FG 代码：

1. ✅ **已存在 ≥2 个独立 fullscreen 效果 Pass（不含 Final）** — BloomExtractPass(§S1a) + BloomBlurPass(§S1b) 已 ship，2 个。
2. ✅ **第三次手写 half-res FBO + ping-pong** — BloomBlurPass 内部 half-res `_pingFbo` + `_pongFbo` 两块 + horizontal→vertical 真 ping-pong（§S1b,2026-07-23）。
3. ❌ **画质选项需要「关效果即不分配 RT」** — 当前 `bloomStrength=0` 仍执行 pass + FBO ensure（虽字节一致但 RT 仍分配）。下一刀候选 = Depth-aware Haze（半分辨率 FS，读 scene depth，可选挂载），把"关效果即不分配 RT"做出来。

**当前状态**：1 ✅ + 2 ✅（已满足「任意两条」最低门槛）。

**决策（2026-07-23）**：

- ⚠️ **仍不立刻开 FG**。理由：
  - FG MVP 的目的 = 替多组"手写 FBO + ping-pong"的 boilerplate；目前只有 Bloom 一族，重复模板不够痛。
  - **先做第 3 条**（Depth-aware Haze 或其它新效果）再开 FG，可以让 FG MVP 直接面对"≥3 个 Pass 共享同一中间 RT"这个真实需求，而不是先造 FG 再去找需求（违反 [`short-term-plan.md` §0 北极星](../short-term-plan.md)）。
  - 中期 FG 范围严格按 [`short-term-plan.md` §6](../short-term-plan.md) 限定：**只迁 Lighting 之后 → Final**，Shadow/GBuffer/Lighting 不动。

**下一刀推荐 = 短期的 §S4：再做一个半分辨率效果**（如简单 Depth-aware Haze）。**做完后再回来开 FG MVP**。

> 在此之前：只维护本文档，代码走 [`short-term-plan.md`](../short-term-plan.md)。

## §S2 SSAO shipped (MVP verification Gate, 2026-07-24)

§A1–§A3 SSAO MVP (mid-term cutsheet SSAO Gate) is now
**shipped**. This is the third mid-term effect to land on the FG
(after Bloom + DepthHaze); it serves as a verification gate that
the FG 套路 works for a brand-new Pass without requiring any
changes to `FrameGraph` core.

### Append-only ABI (no F1–F6 baseline churn)

- `FgResourceId::SSAOTexture = 5` — append before `Count` (now 6)
- `FgSemantic::SSAOSource = 3` — append before `Count` (now 4)
- `RenderPassSlot::SSAO = 11` — append before `Count` (now 12)
- `FrameContext` tail: `ssaoEnabled = false` / `ssaoStrength = 0`
  / `ssaoRadius = 0.5` / `ssaoBias = 0.025` — 4 default-zero knobs

### View-id reservation (single-point bump)

```
BloomExtract = 10   ← A0 (S1a), 不动
BloomBlurH   = 11   ← S1b, 不动
BloomBlurV   = 12   ← S1b, 不动
DepthHaze    = 13   ← S4b, 不动
SSAO         = 14   ← §A1 新增
PostProcess  = 15   ← §A2 单点 bump (10–13 不动)
UI           = 255  ← 不动
```

cutsheet guard: 10–13 不动（cutsheet §FG MVP append-only）。
A2 单点 bump `kBlitViewId` 14→15 一处常量（mirror S4b→S1c
单点 bump 模式）。

### Cutsheet §S2 用户拍板 (locked decisions)

| 维度        | 决策                                | 备注 |
|-------------|-------------------------------------|------|
| Scope       | A — MVP 3 刀（A1 契约/A2 wire/A3 composite） | 默认启用 + Editor 推到 v1 |
| 算法        | A — 8-tap worldPos sphere           | 不重建 normal、不 GTAO |
| 零分配      | A — `!enabled` \|\| `strength<=0` ⇒ 0 alloc | 对齐 Bloom/Haze 套路 |
| 文档        | A — 只动本 cutsheet                 | 不写独立 `docs/ssao.md` |
| saturate    | `clamp(1.0 - x, 0.0, 1.0)`           | Phoskia 无 `saturate` builtin |
| inverse     | 用 builtin `viewProjectionMatrix * vec4(samplePos, 1.0)` 直算 UV | Phoskia 无 `inverse()` |
| composite   | `clamp(1 - aoFactor * strength * step(0.0001, strength), 0, 1)` | branchless 折叠 |
| view id     | Haze=13 → SSAO=14 → PP=15 → UI=255  | PP 单点 bump 一次 |
| Pipeline    | 只挂 Deferred（makeDefault 不插）   | Forward 静默落空 |

### Pipeline position

```
... → BloomExtract → BloomBlur → DepthHaze → SSAO → PostProcess → UI
```

NOTE: composite order = haze-then-AO（DepthHazePass 已 ship
pre-mixed haze RT，S4c 现状；v2 raw-before-haze 需要开新 cutsheet
改 DepthHaze 输出契约 — **不在 MVP 范围**）。

### K-SSAO invariants

| # | Invariant 主题 | 主守位置 | 次守位置 |
|---|---|---|---|
| 1 | ssaoEnabled=false / strength=0 / gbufferPtr=null ⇒ 0 alloc | render() 7 条件 gate | SSAOPass::execute resolve(SSAOTexture) invalid |
| 2 | worldPos.w==0（天空）跳过 occ | SSAOPass FS `step(0.0001, w)` | ── |
| 3 | composite `clamp(1-x, 0, 1)` 而非 `saturate` | PostProcessPass FS | Test pin 字符串 |

### Composite FS contract (Phoskia strings pin)

`PostProcessPass::kPostProcessCacheKey` v6 —
`postprocess_tonemap_aces_v6_prehazed_bloom_ssao_fs`：

```phoskia
material PostProcess {
    texture2d sceneColor
    texture2d bloomTexture
    texture2d hazeTexture
    texture2d ssaoTexture         // ← §A3 第 4 个 sampler (slot 3)
    uniform vec4 ssaoStrength     // ← §A3 strength gate (.x carry)
    ...
    fragment {
        let hazeWeight = step(0.0001, hazeStrength.x)
        let rawHaze = mix(raw, hazeSample.xyz * exposure.x, hazeWeight)
        let ssaoSample = sample(ssaoTexture, uv)
        let aoFactor = clamp(ssaoSample.r, 0.0, 1.0)
        let aoGate = step(0.0001, ssaoStrength.x)
        let aoMul = clamp(1.0 - aoFactor * ssaoStrength.x * aoGate, 0.0, 1.0)
        let rawOccluded = rawHaze * aoMul
        let withBloom = rawOccluded + bloomSample.xyz * bloomStrength.x
        ...
    }
}
```

### SSAOPass FS contract (Phoskia strings pin)

`SSAOPass::kSSAOCacheKey` v1 — `ssao_v1_8tap_worldpos_sphere_fs`：

```phoskia
material SSAO {
    texture2d sceneColor
    texture2d worldPosition     // ← GBuffer RT2 RGBA16F
    texture2d noiseTexture      // ← 4×4 RGBA8 LUT (lazy)
    uniform vec4 ssaoStrength
    uniform vec4 ssaoRadius
    uniform vec4 ssaoBias
    uniform vec4 projection
    uniform vec4 viewportTexel
    fragment {
        let centerWorld = sample(worldPosition, uv)
        let skyGate = step(0.0001, centerWorld.w)
        // 8 taps unrolled (Phoskia no for-loop)
        // let dxN, pN = viewProjectionMatrix * vec4(...), uvN, wN, occN ...
        let occSum = occ0 + ... + occ7
        let occFraction = clamp(occSum * 0.125, 0.0, 1.0)
        let aoOcclusion = clamp(1.0 - pow(clamp(1.0 - occFraction, 0.0, 1.0), 4.0), 0.0, 1.0)
        return vec4(aoOcclusion * skyGate, 0.0, 0.0, 1.0)
    }
}
```

### 验证（每 sub-cut 必跑 3-run stable）

| Sub-cut | 描述 | 3-run stable PASS count |
|---|---|---|
| A1     | SSAO 契约 / 骨架（enum append + 空跑） | **2045/2045 PASS × 3** |
| A2     | Pipeline + FG wire + view id bump 14→15 | **2105/2105 PASS × 3** |
| A3     | Final PostProcess composite | **2127/2127 PASS × 3** |

累计 +82 测试从 F6 baseline 1977（+12 + 60 + 22 = +82/82 PASS）。

### 完成定义

SSAO MVP Gate ship 完成：
- ✅ A1–A3 全部独立 commit landed (3 个独立 commit + 2 个 root pin bump)
- ✅ `frame-graph-mvp.md` §S2 SSAO shipped 段（不新建 `docs/ssao.md`）
- ✅ Default `ssaoEnabled = false` / `ssaoStrength = 0` ⇒ zero alloc verified (K-SSAO-1)
- ✅ View id reservation 钉死：Haze=13 → SSAO=14 → PP=15 → UI=255
- ✅ Composite 用 `clamp(1 - x, 0, 1)` 而非 `saturate` builtin (K-SSAO-3)
- ✅ 2 个 root pin bump commits landed (§A2 后 + §A3 后)
- ✅ memory `ay-renderer.md` 更新

→ 主人另指定 v1 真船（Default enabled + Editor knob +
`docs/ssao.md` 独立 cutsheet）/ 别的工作。
