# Deferred 验收契约（Acceptance Lock）

> 钉住后勿静默回滚。改 GBuffer RT2 / Lighting cache key / Skybox kind 规则必须 bump key **并**改本文件。  
> 日期：2026-07-23 · 对应代码：`gbuffer_fill_v7` / `lighting_v23` / `skybox_v2`

## 管线顺序（Deferred）

`Shadow → Skybox → GBuffer → Lighting → Transparent → PostProcess → UI`

Editor：`AY_DEFERRED=1`。

## 健康日志清单（必须同时出现）

| 关键字 | 含义 |
|--------|------|
| `gbuffer_fill_v7_worldpos_rgba16f` | RT2 = worldPos RGBA16F（阴影 PCF 依赖） |
| `lighting_v23_vec4_ibl_gates` | 点/聚光 + IBL ambient；`cubeActive`/`ambientStrength`/`skyKind` 均为 **vec4** |
| `skybox_v2_vec4_skykind` | equirect / cube 双路径；`skyKind` 为 vec4 |
| `dirs+colors+params+spotDir` | P5.5 B field-split 上传（勿改回仅 dirs+colors） |
| `Skybox equirect + IBL envCube ready` | 天空 = 全景；IBL = 独立 cube（**不要** `kind=CubeMap` 顶掉 equirect） |
| `sceneLights=4` + `point+spot` | keyDir + fillDir + point + spot；**仅 lights[0] 带影** |
| `ShadowPass` … `sampleReady=1` | 阴影 map 可采样 |
| `PostProcessPass` … `gamma=2.2` | 显示 gamma |

## 硬契约（禁止回滚）

1. **GBuffer RT2** = `vec4(worldPos, 1.0)` + FBO **RGBA16F**（不是 motion NDC / 不是 RGBA8）。  
2. **Lighting 阴影**只读 `ctx.shadowPass`；worldPos 来自 `sample(gbufferMotion).xyz`。  
3. **UBO 字段名**保持 `dirs`（不要 `record`）；加灯走 `params` / `spotDir`。  
4. **标量 uniform** 一律 `uniform vec4`，FS 用 `.x`（禁止再嵌 `uniform float`）。  
5. **天空 vs IBL**：`SkySourceKind::Equirect` + `setSkySourceCube(handle)` 可并存；`skyKind` 跟 kind，`cubeActive` 只跟 cube handle。  
6. **单测** cache-key 必须对照 live `k*CacheKeyCStr`，禁止 mirror 自比自。

## Editor 默认观感旋钮

```cpp
setPostProcessGamma(2.2f);
setAmbientStrength(0.85f);  // IBL diffuse 略抬，便于肉眼确认
```

点/聚光勿放进 `lights[0]`（会丢 key shadow）。

## Transparent

`AYRenderSystem` 已按相机距离写 `DrawItem::sortKey`（远→近）。单玻璃物体无需手调；多透明时依赖该自动距离键。

## 回归时先查

若阴影没了 → 先看是否还是 `gbuffer_fill_v7` + Lighting `v23`。  
若天空变纯灰 → 是否误设 `kind=CubeMap` 盖掉 equirect。  
若点/聚光无效果 → 日志是否仍是旧的 `dirs+colors`（无 params）。
