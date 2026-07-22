# PostProcess Pass（显示 Gamma）

> 与 [`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) 配套。  
> 管线位置：`Shadow → FO → Transparent → **PostProcess** → UI`（Deferred 时源为 LightingOutput）。

## 目标

在 Game View 最终 blit 时做**显示 gamma 编码**（近似 sRGB OETF），替代早期用于验槽位的屏幕波纹扰动。

## 效果

- 采样 `sceneColor`（场景 / LightingOutput FBO 颜色附件）。
- `encoded = pow(max(color * exposure + bloom, 0), 1 / gamma)`。
- 默认 `gamma = 2.2`；设为 `1.0` 即线性直通（无编码）。

## API

```cpp
renderer.setPostProcessGamma(2.2f);   // 显示 gamma；1.0 = 线性
renderer.setPostProcessExposure(1.0f);
```

Editor 入口：`EditorPlayRuntime::applyEditorRenderPipeline()` 在配置管线后设置默认值。

## Shader 契约（vec4 ABI）

Cache key：`postprocess_gamma_v1_yflip_fs`（改源必 bump）。

| Uniform / texture | 类型 | 用法 |
|-------------------|------|------|
| `sceneColor` | `texture2d` | 场景色 |
| `bloomStrength` | `vec4` | `.x` |
| `exposure` | `vec4` | `.x` |
| `tonemapMode` | `vec4` | `.x`（本版未分支 tonemap，保留槽位） |
| `uTime` | `vec4` | `.x` = 秒（保留） |
| `gammaParams` | `vec4` | `.x` = display gamma |

bgfx 上传一律按 **Vec4 slot**；Phoskia 侧声明 `vec4` 再 swizzle。详见 Shadow lessons §3.1。

## 验收

1. AYEditor Play：阴影/光照正常，画面不再有波纹扭曲。
2. 日志：`[PostProcessPass] blit ok ... gamma=2.2`，cacheKey 含 `postprocess_gamma_v1`。
3. `setPostProcessGamma(1.0f)`：线性更“灰/洗”；`2.2` 对比更正常。
4. 单测：`AYRenderer_PostProcessPass_R51`。

## 已知简化

- 未做真正 bloom downsample；`bloomStrength` 仍是局部增益近似。
- Tonemap 分支暂未接回 fragment。
- 这是简易 `pow(1/γ)`，不是完整 sRGB 分段曲线。
