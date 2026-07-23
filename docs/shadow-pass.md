# Shadow Pass 使用说明

> 工业级 shadow 子系统。对齐 bgfx `examples/16-shadowmaps` 深度语义，  
> 接入 AY `RenderPipeline`：`Shadow → ForwardOpaque → Transparent → PostProcess → UI`。  
> **Pass 通用踩坑与后续 Pass 清单**见 [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md)。  
> **当前优先级 / 下一步**见 [`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)。  
> **PostProcess 波纹滤镜**见 [`post-process.md`](post-process.md)。

## 快速启用

```cpp
renderer.configurePipeline(RenderPipelineDesc::makeForwardWithShadows());
```

Editor Play 默认已配置该 pipeline。产物 stamp 见 stderr（当前约 `v13-phase7-vec4-abi`）。

## 场景数据

| 物体 | `MeshComponent` | `DrawItem::shadowFlags` |
|------|-----------------|-------------------------|
| 立方体（caster+receiver） | `castShadow=true`, `receiveShadow=true` | `Cast \| Receive` |
| 地面（仅接收） | `castShadow=false`, `receiveShadow=true` | `Receive` |

`RenderSystem` 自动把组件标志映射到 `DrawItem::shadowFlags`。  
**不要**再用 scale 启发式跳过 ground。

## Receiver 材质契约

Phoskia 材质需声明（见 `AYShadowReceiverContract.h` / `kSimpleLitShadowPhoskiaSource`）。  
**bgfx 目标下灯光/bias 用 vec4 + swizzle**（与 hand `.sc` 对齐）：

```
texture2d shadowMap
uniform mat4 u_lightViewProj
uniform vec4 lightDir
uniform vec4 lightColor
property shadowBias     = vec4(0.003, 0.0, 0.0, 0.0)
property shadowDebugVis = vec4(0.0, 0.0, 0.0, 0.0)
property shadowPcf      = vec4(1.0, 0.0, 0.0, 0.0)
```

着色器内：`lightDir.xyz`、`shadowBias.x` 等。  
深度：`ndc01 = (z/w) * 0.5 + 0.5`（R 通道复刻；与 caster / `ShadowDepthCodec` 一致）。

无 `shadowMap` 的材质：shadow bind 为 no-op。  
无 `Receive` 标志的 draw：绑定 lit fallback，保持全亮。

## 环境变量

| 变量 | 作用 |
|------|------|
| `AY_SHADOW_USE_SC=1` | 强制 hand `.sc` 金标（默认走 Phoskia） |
| `AY_SHADOW_USE_PHOSKIA=0` | 同上（旧兼容） |
| `AY_SHADOW_LOG=0..4` | 诊断级别（见下） |
| `AY_SHADOW_DEBUG=1` | receiver 可视化 shadow map 灰度；未设 `AY_SHADOW_LOG` 时默认升到 L3 |
| `AY_SHADOW_CASTER_SOLID=1` | caster 写死 0.5，验证 map 是否写入 |
| `AY_SHADOW_PCF=0` | 关 3×3 PCF（硬影） |
| `AY_MSAA=0` | 关 MSAA（会重建 scene/shadow FBO） |

### 诊断级别 L0–L4

| Level | 内容 |
|-------|------|
| **L0** | 静默 |
| **L1** | 启动一次：`build=` + caps（默认） |
| **L2** | 前 N 帧：`[ShadowPass] frame=... atlasSlots=... sampleReady=...` (§S2-3,2026-07-23) |
| **L3** | CPU LVP 探针：`cubeCenter` / `groundUnderCube` 的 `refN` / `uv` |
| **L4** | 逐 draw：`cast draw#`、`[ShadowBind]` |

示例（PowerShell）：

```powershell
$env:AY_SHADOW_LOG="3"
$env:AY_SHADOW_DEBUG="1"
D:\Projects\out\build\x64-Debug\AYRuntime\AYEditor\AYEditorShell_Demo.exe
```

## 验收清单（Editor Play）

1. stderr 含当前 `build=` stamp；默认 `via Phoskia` / `program ready via Phoskia`
2. 裸跑观感与 `$env:AY_SHADOW_USE_SC="1"` 一致（橙立方体 + 灰地 + 接触影）
3. `groundUnderCube`：`refN≈0.45–0.60`（L3）
4. `AY_SHADOW_DEBUG=1`：cube 脚下非全白
5. `AY_SHADOW_CASTER_SOLID=1`：map 区域约 0.5 灰
6. `AYRenderer_Test` shadow / `ShadowPhoskiaEmit` 相关用例全绿

## 架构速查

```
ShadowPass
  ├─ ShadowMatrixBuilder   scene AABB → light view + ortho
  ├─ ShadowMapResources    RGBA8 + D24S8 FBO → blit resolve
  ├─ ShadowCaster          castsShadow(flags) 的 depth draw
  └─ ShadowDiagnostics     L0–L4 + ShadowFrameStats

ForwardOpaque / Transparent
  └─ tryBindShadowSampler(shader, adapter, shadowPass, item.shadowFlags)
```

## 相关头文件

| Header | 角色 |
|--------|------|
| `AYShadowSettings.h` | 常量 + build stamp |
| `AYShadowDepthCodec.h` | CPU ndc01 / pack / compare |
| `AYShadowShaderSources.h` | caster + receiver Phoskia |
| `AYShadowReceiverContract.h` | receiver 绑定契约 |
| `AYShadowDiagnostics.h` | 日志级别 + frame stats |
| `AYShadowConfig.h` | 伞头 |

## 已知限制 / 后续

- 深度为 **R8 复刻**（非 `packFloatToRgba`）；clear=`0xffffffff` + `step(0.999,o)` 契约见 lessons 文档 §3.6
- 已有可选 3×3 PCF（`shadowPcf` / `AY_SHADOW_PCF`）；VSM / ESM / PCSS / CSM 未做
- FO 采样 caster color RT；blit→resolve 仅可选 readback（须独立 view）
- 单 cascade 方向光 ortho；扩展预留见 `docs/shadow-pass-plan.md`
- 产品默认 pipeline **可不含** Shadow；Editor / demo 用 `makeForwardWithShadows()`

## Shadow Bias 控制

- Phoskia receiver 侧声明 `property shadowBias = vec4(0.003, 0.0, 0.0, 0.0)`（`AYShadowShaderSources.h`）；receiver fragment 在 `refNdc01 + bias` 之后做深度比较（`AYShadowShaderSources.h:211`）。
- **P4.2（§P4, 2026-07-22）**：新增全局 bias CPU 镜像 ── `FrameContext::shadowBias` + `Renderer::setShadowBias(float)` / `shadowBias()` getter。Host 一处控制，影响所有用 shadow 的 receiver 材质。
- 默认 `0.003f`，匹配 Phoskia property 默认 + `ShadowSettings::kBiasDefault`。
- `tryBindShadowSampler(shader, adapter, shadowPass, flags, bias)` 多 5 参 `bias`（默认 `0.003f`，向后兼容），`ForwardOpaquePass` + `TransparentPass` 调用点都传 `frame.shadowBias`。
- Per-material 覆写仍可用 `setMaterialVec3(material, "shadowBias", v)`；全局值在 per-material uniform write 之后覆盖（可在将来加门控逻辑）。
- 取值建议：`0`（关闭）→ `0.001`→ `0.005`（强）。负值技术上接受，常见接收 shader 产生「背面阴影」伪影。`peter-panning`（阴影脱离物体）通常 `bias >= 0.008` 触发。
- 单测：`unittest/Test_P4_ShadowBias.cpp`（4 case：FrameContext 默认 / Renderer getter 默认 / set-get 往返 / tryBindShadowSampler 接受 5 参）。
