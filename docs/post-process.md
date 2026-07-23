# PostProcess（Final + 显示 Gamma + 真 Bloom 合成）

> 与 [`renderer-pass-roadmap.md`](renderer-pass-roadmap.md) · [`frame-graph-mvp.md`](frame-graph-mvp.md) · [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) 配套。
> 日期：2026-07-23 · 当前 cache key：`postprocess_tonemap_aces_v3_bloom_composite_fs`
> 短期计划：见 [`short-term-plan.md`](short-term-plan.md) §S1。

## 1. 管线位置

```
Shadow → [Skybox → GBuffer → Lighting → Transparent] → BloomExtract → BloomBlur → **PostProcess(Final)** → UI
```

PostProcess 是整支管线的**最后一个 blit**(到默认 backbuffer 或 Editor panel hole)。源 FBO 由 `PostProcessPass::selectSourceFbo()` 选：

1. `ctx.lightingPass->lightingOutputFbo()`（Deferred 路径，已挂 LightingPass 且 FBO ensure 过）
2. `ctx.sceneFbo`（Forward / 默认路径）
3. 都不 valid → execute() 早返回 0（no-op）

## 2. 现在的合成契约（S1c, 2026-07-23）

**单一 Final FS, 两个 sampler, 四个标量 vec4**：

| Sampler / uniform | 用途 |
|---|---|
| `sceneColor` (slot 0) | LightingOutput / scene FBO RT0 |
| `bloomTexture` (slot 1) | `ctx.bloomBlurPass->pongFbo()` RT0（真 bloom 半分辨率高斯模糊） |
| `bloomStrength` (vec4 .x) | 0 = 关闭 bloom；Editor 默认 0.2~0.4 |
| `exposure` (vec4 .x) | 标量乘 sceneColor |
| `tonemapMode` (vec4 .x) | 0=None / 1=Reinhard / 2=ACES（branchless `mix(mix(none,rein,step(.5,m)),aces,step(1.5,m))`） |
| `gammaParams` (vec4 .x) | display gamma（`encoded = pow(max(mapped,0), 1/γ)`） |
| `uTime` (vec4 .x) | 保留（诊断） |

**核心公式**（post-process.cpp:76-77 锚点）：

```
raw        = sampled.xyz * exposure.x
withBloom  = raw + bloomSample.xyz * bloomStrength.x
cx,cy,cz   = max(withBloom.{x,y,z}, 0)
{selX, selY, selZ} = branchless tonemap mix(...) on {cx,cy,cz}
encoded    = vec3(pow(max(selX,0), 1/γ), pow(max(selY,0), 1/γ), pow(max(selZ,0), 1/γ))
return     = vec4(encoded, sampled.w)
```

UV 在 FS 里手动 y-flip（`uv = vec2(vUv.x, 1.0 - vUv.y)`）：Phoskia vertex block 当前禁止 `let` 在 `out` 之前；同 Shadow 路径 D3D RT 与 backbuffer 约定。

### 2.1 K3 不变量（短路 / producer-absent 守）

| 情况 | 结果 |
|---|---|
| `bloomTexture` sampler 未绑（`ctx.bloomBlurPass == nullptr` 或 `pongFbo()` invalid） | bind `sceneColor` 到 bloom slot → FS `bloomSample == sampled` → `withBloom = raw * (1 + bloomStrength)`；若 `bloomStrength=0` ⇒ 与关 Bloom **字节一致** |
| `bloomStrength=0` | `withBloom = raw` ⇒ 与关 Bloom **字节一致** |
| TonemapMode=None + gamma=1.0 | 线性直通；对比更"灰/洗" |

> 这三条保证：未挂 BloomExtract/BloomBlur 的 host（custom desc 省略它们）走 PostProcess 零视觉差异 ⇒ pre-S1 验收锁不破。

## 3. 配套 Pass 族（§S1 短期链）

### 3.1 BloomExtractPass（§S1a）

- 管线位置：Transparent 之后，PostProcess 之前
- 输入：`sceneColor` (full-res)
- 输出：half-res RGBA8 FBO `_fboColor`，仅含高亮（threshold + soft knee，留在短期后续 polish 时再细调；S1a MVP 用 `max(color - 1.0, 0)` 简化版）
- ViewId：`kBloomExtractViewId=10`
- Cache key bump：每次改 shader 必 bump

### 3.2 BloomBlurPass（§S1b）

- 输入：`BloomExtractPass::_fboColor`
- 输出：横纵 5-tap Karplus-Strong-ish Gaussian 半分辨率 ping-pong
  - view 12：horizontal `_pingFbo → _pongFbo`
  - view 13：vertical   `_pongFbo → _pingFbo`
- Weights：`[0.227, 0.194, 0.121, 0.054, 0.016]`
- Final 半分辨率产物在 `_pingFbo` RT0（vertical pass 后）
- PassExecContext 暴露：`bloomExtractPass` / `bloomBlurPass` 两个 borrowed ptr

### 3.3 PostProcess 合成（§S1c） ← 本文件核心

见 §2。

### 3.4 Editor 调（§S1d）

```cpp
renderer.setPostProcessBloomStrength(0.2f ~ 0.4f);  // 默认略开
// 0 = 关；Editor Play 启动时由 AYEditorPlayRuntime::applyEditorRenderPipeline() 设置
```

## 4. API

```cpp
renderer.setPostProcessGamma(2.2f);
renderer.setPostProcessExposure(1.0f);
renderer.setPostProcessTonemapMode(Renderer::TonemapMode::ACES);
renderer.setPostProcessBloomStrength(0.0f);  // 0 = 关；§S1d Editor 默认略开
```

Editor 入口：`AYEditorPlayRuntime::applyEditorRenderPipeline()` 在配置管线后统一设置默认值。

## 5. Shader ABI 契约（vec4 + bgfx Vec4 slot）

| 通道 | 类型 | 说明 |
|------|------|------|
| `sceneColor` | `texture2d` slot 0 | 场景色 |
| `bloomTexture` | `texture2d` slot 1 | 真 bloom 半分辨率结果 |
| `bloomStrength` | `uniform vec4` | `.x` = 强度 |
| `exposure` | `uniform vec4` | `.x` = 标量 |
| `tonemapMode` | `uniform vec4` | `.x` = 0/1/2 |
| `uTime` | `uniform vec4` | `.x` = 秒（保留） |
| `gammaParams` | `uniform vec4` | `.x` = display gamma |

bgfx 一律 Vec4 slot；Phoskia 侧声明 `vec4` 再 `.x` swizzle。详见 Shadow lessons §3.1。

Cache key 历次 bump：
- `v1_aces_yflip_fs` — 初版 y-flip 修
- `v2_yflip_fs` — 命名规范化
- **`v3_bloom_composite_fs`（当前）** — §S1c 加 `bloomTexture` 第二 sampler + 真合成

## 6. 验收（Editor Play）

1. `[PostProcessPass] blit ok view=4 rect=(...) gamma=2.2 ... tonemap=2 bloom=0.20 bloomSrc=pong` —— bloomSrc=pong 表示真合成路径生效。
2. `bloomSrc=fallback(scene)`：bloomStrength=0 或 producer-absent；与关 Bloom **字节一致**。
3. cacheKey 含 `v3_bloom_composite`。
4. 关 Bloom（`bloomStrength=0`）：Deferred 验收日志仍全绿（参考 [`deferred-acceptance.md`](deferred-acceptance.md)）。
5. 单测：`AYRenderer_Test` 内 `Test_FinalPP_S1c` + `Test_PostProcess_R51` + `Test_BloomBlur_S1b`。

## 7. 已知简化（短期不做）

- 完整 sRGB 分段曲线（仍是 `pow(1/γ)` 简化）
- Bloom threshold soft-knee 精细调（S1a MVP 用 `max(color - 1.0, 0)`）
- 双 Kawase blur / Karis filter / radius knob（S1b 当前 5-tap Karplus-Strong-ish Gaussian）
- Bloom 强度自适应（曝光感知）
- 多效果链（SSAO / TAA / DOF）— 等中期 FrameGraph MVP