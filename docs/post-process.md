# PostProcess Pass（Tonemap + 显示 Gamma）

> 与 [`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) 配套。  
> 管线位置：`Shadow → FO → Transparent → **PostProcess** → UI`（Deferred 时源为 LightingOutput）。

## 目标

在 Game View 最终 blit 时做 **HDR tonemap + 显示 gamma 编码**（近似 sRGB OETF），替代早期验槽位波纹扰动。

## 效果

- 采样 `sceneColor`（场景 / LightingOutput FBO 颜色附件）。
- `exposed = color * exposure`（可选假 bloom：`+ raw * bloomStrength`）。
- **Tonemap**（branchless，`tonemapMode.x`）：
  - `0` None — 直通
  - `1` Reinhard — `c / (1+c)`
  - `2` ACES — Narkowicz fitted curve
- `encoded = pow(max(mapped, 0), 1 / gamma)`。
- 默认 API：`gamma=2.2`、`tonemap=None`；Editor Deferred 默认 **ACES**。

## API

```cpp
renderer.setPostProcessGamma(2.2f);   // 显示 gamma；1.0 = 线性
renderer.setPostProcessExposure(1.0f);
renderer.setPostProcessTonemapMode(Renderer::TonemapMode::ACES);
```

Editor 入口：`EditorPlayRuntime::applyEditorRenderPipeline()` 在配置管线后设置默认值。

## Shader 契约（vec4 ABI）

Cache key：`postprocess_tonemap_aces_v2_yflip_fs`（改源必 bump）。

| Uniform / texture | 类型 | 用法 |
|-------------------|------|------|
| `sceneColor` | `texture2d` | 场景色 |
| `bloomStrength` | `vec4` | `.x` |
| `exposure` | `vec4` | `.x` |
| `tonemapMode` | `vec4` | `.x` = 0/1/2 |
| `uTime` | `vec4` | `.x` = 秒（保留） |
| `gammaParams` | `vec4` | `.x` = display gamma |

bgfx 上传一律按 **Vec4 slot**；Phoskia 侧声明 `vec4` 再 swizzle。详见 Shadow lessons §3.1。

## 验收

1. AYEditor Play：阴影/光照正常；比纯 gamma 更有高光压暗、对比更稳。
2. 日志：`[PostProcessPass] blit ok ... gamma=2.2 ... tonemap=2`，cacheKey 含 `postprocess_tonemap_aces_v2`。
3. `TonemapMode::None` + `gamma=1.0`：线性更“灰/洗”；ACES+2.2：对比更正常。
4. 单测：`AYRenderer_PostProcessPass_R51`。

## 已知简化

- 未做真正 bloom downsample；`bloomStrength` 仍是局部增益近似。
- 这是简易 `pow(1/γ)`，不是完整 sRGB 分段曲线。
- 多 Pass 效果（真 Bloom / SSAO / TAA）应另开 Pass，再合成进本 Final blit。
