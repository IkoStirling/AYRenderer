# PostProcess Pass（波纹滤镜）

> 与 [`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) 配套。  
> 管线位置：`Shadow → FO → Transparent → **PostProcess** → UI`。

## 目标（本迭代已落地）

在 AYEditor Game View 上加一层**可见**后处理：画面随时间做屏幕空间波纹扰动，用来证明 PP 槽位在真实 GPU 路径上在跑（而不只是 blit 直通）。

## 效果

- 采样 `sceneColor`（场景 FBO 颜色附件）。
- 以屏幕中心为圆心，按距离做：

  `wave = sin(dist * frequency - time * speed)`  
  `uv' = uv + normalize(uv - center) * (wave * strength)`

- `rippleStrength == 0` → UV 不变，等价于曝光/bloom 直通（默认 0，不破坏旧宿主）。
- Editor Play 默认打开一小强度，便于肉眼确认。

## API

```cpp
renderer.setPostProcessRippleStrength(0.012f);  // UV 偏移幅度；0 = 关
renderer.setPostProcessRippleFrequency(28.0f);  // 空间频率
renderer.setPostProcessRippleSpeed(4.0f);       // 时间速度
```

时间来自 `FrameContext::timeSeconds`（`Renderer` 内 steady_clock）。

Editor 入口：`EditorPlayRuntime::applyEditorRenderPipeline()` 在 `configurePipeline(makeForwardWithShadows())` 之后设置上述默认值。

## Shader 契约（vec4 ABI）

Cache key：`postprocess_ripple_v1_vec4`（改源必 bump）。

| Uniform / texture | 类型 | 用法 |
|-------------------|------|------|
| `sceneColor` | `texture2d` | 场景色 |
| `bloomStrength` | `vec4` | `.x` |
| `exposure` | `vec4` | `.x` |
| `tonemapMode` | `vec4` | `.x`（本版未分支 tonemap，保留槽位） |
| `uTime` | `vec4` | `.x` = 秒 |
| `rippleParams` | `vec4` | `.x` strength，`.y` frequency，`.z` speed |

bgfx 上传一律按 **Vec4 slot**；Phoskia 侧声明 `vec4` 再 swizzle，勿依赖 GLSL uniform 初值。详见 Shadow lessons §3.1。

## 验收

1. 裸跑 AYEditor Play：橙色立方体 + 阴影，画面有**随时间扩散/涟漪**的轻微扭曲。
2. `setPostProcessRippleStrength(0)`：扭曲消失，场景仍正常。
3. 单测：`AYRenderer_PostProcessPass_R51`（源串 / binding 名 / setter）。

## 已知简化

- 未做真正 bloom downsample；`bloomStrength` 仍是局部增益近似。
- Tonemap 分支暂未接回 fragment（converter 对 `if` 不友好时优先保证波纹可见）。
- 强度过大（>0.05）易出现边缘采样拉伸；Editor 默认约 `0.012`。
