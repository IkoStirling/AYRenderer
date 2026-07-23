# AYRenderer 短期执行计划（Short-Term）

> **唯一开工清单。** 不在本文件「本周/下周」条目里的事 → 默认不做。  
> 配套：[`deferred-acceptance.md`](deferred-acceptance.md) · [`frame-graph-mvp.md`](frame-graph-mvp.md) · [`post-process.md`](post-process.md)  
> 日期：2026-07-23 · 基线：Deferred 已绿（`lighting_v26` + ACES Final PP）

---

## 0. 北极星（短期只服务这个）

**在现有显式 Pass 管线上，把观感再抬一档，并练熟「半分辨率效果 → Final 合成」模式——为中期 FG 攒真实需求，而不是先造 FG。**

当前管线（勿改拓扑）：

```
Shadow → Skybox → GBuffer → Lighting → Transparent → PostProcess(Final) → UI
```

---

## 1. 禁止清单（短期红线）

| 禁止 | 原因 |
|------|------|
| 实现 FrameGraph / RenderGraph | 中期，见 `frame-graph-mvp.md` |
| SSAO / SSR / TAA / DOF / Motion Blur | 吃 GBuffer/历史帧，属中后期 |
| CSM / PCSS / 多级阴影 atlas 产品化 | Shadow Round 2 |
| 整帧迁 FG、改 `makeDeferred()` 槽位大重组 | 破坏已钉契约 |
| 重写 Phoskia if/for 运行时 | 正交大坑，另开专项 |
| 「假 Bloom」继续当真 Bloom 卖 | 短期应做真 downsample 链或明确不碰 |
| 无 bump cache key 改 shader | 违反 lessons |

若某任务需要动以上任一项 → **停**，先改本计划或升到中期 cutsheet。

---

## 2. 已完成（不要回头重做）

- [x] Deferred 闭环 + 验收锁（GBuffer RT2 worldPos RGBA16F）
- [x] Lighting 点/聚光 + IBL ambient + key shadow
- [x] Final PP：ACES tonemap + gamma + exposure
- [x] Editor：equirect 天空 + 独立 IBL cube + `ambientStrength(0.85)`
- [x] AYShader：`uniform T name[N]`、`mat4` let、`mix(vec2,…)`

健康日志仍以 [`deferred-acceptance.md`](deferred-acceptance.md) 为准（Lighting key 以 live `kLightingCacheKeyCStr` 为准）。

---

## 3. 执行顺序（严格按刀）

### 刀 S0 — 钉文档与防漂移（0.5 天）

- [x] `docs/short-term-plan.md`（本文件）
- [x] `docs/frame-graph-mvp.md`
- [x] 修复/对齐 `deferred-acceptance.md`（`lighting_v26` + 编码）
- [x] `renderer-pass-roadmap.md` 顶部加「当前执行：short-term-plan」链接

**完成标准**：任何人打开 roadmap 知道「下一刀是 S1，不是 FG」。 → **已满足，下一刀 = S1a。**

---

### 刀 S1 — Bloom MVP（真半分辨率链，手写 FBO）（3–5 天）

**唯一允许新增的 Pass 族。** 插在 `Transparent` 与 `PostProcess(Final)` 之间。

建议切片（每刀可独立提交）：

| 子刀 | 内容 | 验收 |
|------|------|------|
| S1a | `BloomExtractPass`：读 SceneColor/LightingOutput，写 half-res bright | 日志 + 单测 Noop 不崩 |
| S1b | `BloomBlurPass`：half ping-pong（可先 1 次横 + 1 次纵） | 画面有朦胧高光 |
| S1c | Final PP：采样 bloom 纹理合成（替换假 `color+color*bloomStrength`） | `bloomStrength=0` ≈ 关 Bloom |
| S1d | Editor：`setPostProcessBloomStrength(0.2~0.4)` 默认略开 | 肉眼可辨；可关 |

**实现约束（为中期 FG 铺路，但不是 FG）：**

- FBO 生命周期：`ensure(w/2,h/2)`，跟 viewport resize。
- viewId：紧挨现有 PP blit（文档写死占用表，勿与 Shadow/GBuffer 撞）。
- 一律 `uniform vec4` + cache key bump。
- **不要**引入资源图、不要自动 alias。

**完成标准**：Bloom 开关可感；关 Bloom 时 Deferred 验收日志仍全绿。

---

### 刀 S2 — 小抛光（1–2 天，可选，S1 之后）

按优先级，**最多做两项**：

1. Exposure 微调默认（ACES 下若偏暗/偏亮）  
2. Final 轻微 vignette **或** contrast（仍在 Final 一个 FS，禁止新 Pass）  
3. Shadow 日志：`draws=` **已改名为** `atlasSlots=`（§S2-3,2026-07-23；避免再误解；**不改阴影算法**）

**完成标准**：观感更稳；无新大系统。

---

### 刀 S3 — 短期收口（0.5–1 天）

- [x] 更新 [`post-process.md`](post-process.md)：真 Bloom 链 + Final 合成契约（§S1a/S1b/S1c/S1d 全部反映,K3 不变量锁死）
- [x] 在 [`frame-graph-mvp.md` §7](frame-graph-mvp.md) 打勾：BloomExtract + BloomBlur = 2 个效果 Pass ✅;半分辨率 ping-pong = 第三次 half FBO ✅;**已满足"任意两条"门**
- [x] 决定下一阶段：**先做 S4（Depth-aware Haze）再做 FG MVP**

**结论（2026-07-23）**：S3 收口完成。**下一刀 = S4 Depth-aware Haze 半分辨率效果 Pass**（理由见 [`frame-graph-mvp.md` §7 决策](../frame-graph-mvp.md)）。FG MVP 推到 S4 完成后再开。

---

## 4. 「下一刀是什么」速查

| 你现在的状态 | 下一刀 |
|--------------|--------|
| 刚读完本文 | **S0 收尾**（修 acceptance 编码 + roadmap 链接） |
| S0 完 | **S1a BloomExtract** |
| S1a 完 | **S1b Blur** |
| S1b 完 | **S1c Final 合成** |
| S1 全完 | **S2 任选 ≤2 项** 或直接 **S3** |
| 想做 FG / SSAO / TAA | **停** → 读 `frame-graph-mvp.md`，改计划后再动 |

---

## 5. 每日开工检查（防跑偏）

开工前问三句：

1. 这一刀是否写在上面 S0–S3？  
2. 是否需要新 transient RT 拓扑自动化？→ 要则 **中期 FG**，短期用手写。  
3. 是否可能弄坏 `gbuffer_v7` / Lighting live key / equirect+IBL？→ 先跑验收日志。

---

## 6. 中期预告（只读，勿做）

见 [`frame-graph-mvp.md`](frame-graph-mvp.md)：只把 **Lighting 之后 → Final** 迁入轻量 FG；Shadow/GBuffer/Lighting 不动。
