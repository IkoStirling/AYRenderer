# GBuffer 当前架构与数据契约

> 当前契约：`material-contract-v3-model-coverage-split`，适用于 Deferred 路径。

## 1. Pass 责任

`GBufferPass` 是延迟渲染的几何生产者：遍历不透明/剪切物体，使用固定的 GBuffer Fill shader 将几何和材质数据写入四张颜色附件与一张深度附件。它拥有 FBO/附件，下游 Pass 只借用句柄。

Pass 有两层有效性：

- `isReady()`：附件已完整分配。
- `producedThisFrame()`：本帧已成功排入 GBuffer clear/draw。下游以此阻止冷启动、缩放或失败帧误读旧数据。

## 2. MRT 布局

| 附件 | 格式 | RGB/XYZ | A | 主要消费者 |
|---|---|---|---|---|
| RT0 | RGBA8 | Albedo | Metallic | Lighting, Debug |
| RT1 | RGBA8 | 世界法线，编码至 `[0,1]` | Roughness | Lighting, SSAO, Debug |
| RT2 | RGBA16F | World position | AO + MaterialModel 打包值 | Lighting, SSAO, DepthHaze, Debug |
| RT3 | RGBA8 | Emissive | Geometry coverage | Lighting, Debug |
| Depth | D24S8 | 不透明/剪切深度 | — | MotionVector（借用）、Debug |

颜色 MRT 总带宽为 160 bpp，本轮没有增加附件或像素带宽。附件索引和 CPU 编解码集中在 `GBufferLayout`。

RT2.a 的编码为：

```text
packed = materialModel * 2.0 + clamp(AO, 0.0, 1.0)
materialModel = floor(packed / 2.0)
AO = clamp(packed - materialModel * 2.0, 0.0, 1.0)
```

当前材质模型：

- `StandardLit = 0`：标准 PBR 光照。
- `Unlit = 1`：输出 `albedo + emissive`，不参与光照。

RT3.a 仅表示几何覆盖率，不再与材质模型复用；Lighting 仍使用它区分几何和背景。

## 3. 数据流

```text
RenderAssetBridge / AYRenderer API
        │  GpuMaterial + MaterialModel
        ▼
RenderScene DrawItem
        │
        ▼
GBufferPass (view 7)
        ├─ RT0/RT1/RT2/RT3 ──► LightingPass (view 8) ──► Deferred color
        │          ├──► SSAOPass
        │          ├──► DepthHazePass
        │          └──► GBufferDebugPass (view 250)
        ├─ Depth ──► MotionVectorPass (view 3, color-only replay)
                   └─ RG16F velocity ──► TAAPass
        └─ Depth ───────────────────────────► GBufferDebugPass
```

`RenderAssetBridge` 优先读取保留的 `__ayMaterialModel` 整数参数；未提供时，shader 路径包含 `unlit` 的材质自动归类为 `Unlit`，其余默认为 `StandardLit`。运行时也可显式设置：

```cpp
renderer.setMaterialModel(material, MaterialModel::Unlit);
```

## 4. 本轮未采用的改动

- **删除 RT2 WorldPosition，从 Depth 重建**：当前 D3D 路径已有重建失败的回归记录，且 Lighting、SSAO、DepthHaze 与阴影都依赖世界坐标。在真实 GPU capture 和跨后端验证前保留 RT2。
- **把 Velocity 作为第五张 GBuffer MRT**：未采用。当前由独立 `MotionVectorPass` 在 TAA 启用时按需分配全分辨率 `RG16F`（32 bpp）并借用 GBuffer depth；GBuffer 关闭 TAA 时仍保持 160 bpp、零 velocity 分配。代价是 TAA 路径多一次 opaque/cutout 几何重放，换取默认 Deferred 路径不永久增加 MRT 带宽，也为未来降分辨率/按需消费者保留独立演进空间。

## 5. 调试通道

`GBufferDebugPass` 提供 Albedo、Normal、WorldPos、Material（metallic/roughness/AO）、Depth 和 MaterialModel 六个视图。MaterialModel 视图中 StandardLit 为绿色，Unlit 为红色。`Motion` 仍是 GBufferDebug 的源码兼容别名；独立 MotionVector 纹理尚未接入该面板，后续应新增专门的 velocity 可视化，而不是把它伪装成 GBuffer 附件。
