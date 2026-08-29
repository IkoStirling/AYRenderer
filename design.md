# AYRenderer Design

> **文档状态**：2026-08-29 已对齐当前代码；R0–R5 主管线已落地。
> **实现状态**：Forward 仍为产品默认，Deferred 通过 `makeDeferred()` 显式启用。Shadow、GBuffer、Lighting、Transparent、Bloom、DepthHaze、SSAO、PostProcess、UI 和 GBufferDebug 已接入 Pass 调度。GBuffer 已冻结 `material-contract-v3-model-coverage-split`；Lighting、Transparent、Bloom、DepthHaze 与 SSAO 第二轮按序修复已完成，等待真实 GPU capture 验收。
> **活动执行计划**：[`docs/execution-plan.md`](execution-plan.md)（P0–P6 队列、§5 segfault 约束、§5.4 隔离实验、附录 B/C/D 索引）。本文件是目标架构，与代码不一致时以代码与 execution-plan 为准。
> **关联文档**：[`docs/gbuffer-current.md`](docs/gbuffer-current.md)（当前 MRT/数据契约）、[`AYShader/design.md` §8.5](../AYShader/design.md)（opaque handle contract）、[`AYShader/README.md`](../AYShader/README.md)。

---

## 1. 概述

AYRenderer 负责：

- bgfx 生命周期（init / frame / shutdown）
- 视口、清屏、渲染状态
- 几何资源（VertexBuffer / IndexBuffer / Texture）创建与绑定
- 每帧收集可绘制对象，按 Pass 调度提交
- **通过 AYShader 的 `ShaderResource` 完成 shader 绑定与 program submit**（不自行调用 shaderc，不持有 `bgfx::ProgramHandle`）

### 1.1 设计原则

| 原则 | 说明 |
|---|---|
| **Shader 与 Renderer 解耦** | 材质侧只认 `ShaderResource` + `BindingId`；编译与 bgfx program 创建在 AYShader 内 |
| **bgfx 只在 Renderer 层** | Renderer TU 可 `#include <bgfx/bgfx.h>`；**禁止**在材质/游戏逻辑代码里 spread program/uniform handle |
| **Forward / Deferred 双路径** | Forward 保持默认与回归基线；Deferred 显式启用并作为主要扩展路径 |
| **单线程优先** | R1 直接调用；Command Queue 仅预留接口 |
| **小步可验证** | 每个 phase 有独立测试或 demo 场景 |

### 1.2 在引擎中的位置

```
┌────────────────────────────────────────────────────────────────────┐
│                         AYEngine / AYGameLoop                       │
├────────────────────────────────────────────────────────────────────┤
│  AYDevice                    AYRenderer                             │
│  (SDL 窗口 / 输入)            │                                    │
│       │ windowHandle          ├── BGFXAdapter (frame / vb / ib)    │
│       └──────────────────────►├── RenderPipeline (Pass 调度)       │
│                               ├── RenderScene (帧期场景快照)        │
│                               └── RenderResourceManager (GPU 资源)  │
│                                        │                            │
│                                        │ acquire / setUniform       │
│                                        ▼                            │
│                               ┌─────────────────┐                  │
│                               │  AYShader       │                  │
│                               │ ShaderResource  │                  │
│                               │ Pool            │                  │
│                               └────────┬────────┘                  │
│                                        │ pimpl → bgfx program       │
│                                        ▼                            │
│                               ┌─────────────────┐                  │
│                               │  bgfx           │                  │
│                               └─────────────────┘                  │
└────────────────────────────────────────────────────────────────────┘
```

---

## 2. 与 AYShader 的集成 Contract（必读）

本节是 Renderer 设计的 **spine**；与 [`AYShader/design.md` §8.5](../AYShader/design.md) 一致。

### 2.1 Frontend 应使用的类型

```cpp
namespace ayt::shader {

using BindingId = uint32_t;
constexpr BindingId InvalidBinding = 0;

struct TextureHandle {
    uint64_t id = 0;   // 见 §2.4
    bool isValid() const noexcept { return id != 0; }
};

struct DrawCallContext {
    uint8_t  viewId = 0;
    uint64_t state  = 0;   // bgfx 渲染状态位；0 = 不在 submit 内改 state
};

class ShaderResource;      // opaque；绑定 + submit
class ShaderResourcePool;  // 工厂 + cache + hot-reload + shutdown 回收

} // namespace ayt::shader
```

### 2.2 Frontend 不应使用的类型（材质 / 绑定路径）

| 禁止出现在 RenderMaterial / 游戏层 | 原因 |
|---|---|
| `bgfx::ProgramHandle` | 由 `ShaderResource` pimpl 持有 |
| `bgfx::UniformHandle` | 用 `BindingId` 间接操作 |
| `ShaderProgram`（legacy） | 已迁入 `detail/`；新代码禁用 |
| `BGFXConvertResult` / `.sc` 字符串 | 调试走 `CompileOptions::keepSources` |

Renderer **可以**在资源层使用 `bgfx::VertexBufferHandle` / `bgfx::TextureHandle` 等 **几何与图像** handle；与 shader 路径分离。

### 2.3 编译与获取 ShaderResource

```cpp
#include "AYShader.h"
#include "AYShader/Phoskia.h"

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

// 引擎启动：构造 pool，配置 shaderc / platform（一次）
ShaderResourcePool pool;
pool.setShadercExecutable(".../shaderc.exe");
pool.setPlatform("windows");
pool.setGLSLProfile("430");
pool.setAutoProbeFromRendererType(false);  // 或与 bgfx renderer type 联动
pool.setCacheDirectory(".../shader_cache"); // 可选

// 加载材质：从 Phoskia 源 acquire（带 source + binary 两级 cache）
ShaderResource mat = pool.acquire(phoskiaSource);

// 或经 Compiler 一站式（内部仍进 pool）
Compiler compiler;
ShaderResource mat2 = compiler.compileToShaderResource(phoskiaSource, CompileOptions{}, pool);

// 开发期 hot-reload
pool.setHotReloadEnabled(true);
pool.compileFromFile("shaders/unlit.phoskia");
// 每帧或定时：
pool.pollHotReload();
```

失败时 `ShaderResource::isValid() == false`；Renderer 应跳过 draw 并打日志，不 crash。

### 2.4 TextureHandle 约定（Phase 4  interim）

当前 AYShader 实现将 `TextureHandle.id` 的低 16 位映射为 `bgfx::TextureHandle.idx`：

```cpp
// AYRenderer 侧包装（R1 起放在 RenderTexture 或 BGFXAdapter）
inline shader::TextureHandle toShaderTexture(bgfx::TextureHandle h) {
    shader::TextureHandle out;
    out.id = h.idx;
    return out;
}
```

**后续（R2+）** 可改为 Renderer 维护 `TextureRegistry`，`TextureHandle.id` 为引擎侧 stable id，AYShader 内部再解析——design 预留，R1 沿用 idx 映射即可。

### 2.5 单次 Draw Call 顺序（bgfx 语义）

bgfx 要求 **同一 draw** 的 state / vb / ib / transform / uniform / texture 在 `submit` 之前设置完毕。

```
AYRenderer::drawMesh(...) 推荐顺序：

  1. BGFXAdapter::setViewRect / setViewClear          // 视口级（Pass 入口）
  2. BGFXAdapter::setTransform(worldMatrix)
  3. BGFXAdapter::setVertexBuffer(vb)
  4. BGFXAdapter::setIndexBuffer(ib)                  // 非 indexed 则跳过
  5. ShaderResource::setUniform(...)                  // 可多次
  6. ShaderResource::setUniformBlock(...)             // UBO（可选）
  7. ShaderResource::setTexture(stage, bindingId, tex)
  8. ShaderResource::submit(DrawCallContext{viewId, state})
         └── 内部：flush pending uniform/texture → bgfx::setState → bgfx::submit
```

**职责 split**：

- **BGFXAdapter**：步骤 1–4（几何与 transform）
- **ShaderResource**：步骤 5–8（shader 参数 + program submit）

`DrawCallContext::state` 非 0 时，`submit()` 内会 `bgfx::setState(ctx.state)`。Pass 级 state 与 per-draw state 合并策略在 R1 定为：**以 DrawCallContext 为准**；Adapter 不再单独 `setState` 除非 debug。

### 2.6 Binding 查询示例

Phoskia 声明名与查询名一致（property / uniform / texture / uniformblock 实例名）：

```cpp
ShaderResource& shader = material.shader();

BindingId tintId    = shader.getUniformBinding("tint");
BindingId albedoId  = shader.getTextureBinding("albedoMap");
BindingId cameraId  = shader.getUniformBlockBinding("Camera");

shader.setUniform(tintId, &color, sizeof(color));

shader::TextureHandle th = toShaderTexture(gpuAlbedo);
shader.setTexture(0, albedoId, th);

// UBO：用 getUniformBlockSize / getUniformBlockFieldOffset 填 staging buffer
shader.setUniformBlock(cameraId, cameraBlob, blockSize);

shader.submit({ .viewId = 0, .state = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS });
```

---

## 3. 核心架构

### 3.1 R1 最小模块（先实现这些）

```
AYRenderer
├── BGFXAdapter           # bgfx init/frame/view + vb/ib/transform
├── ForwardOpaquePass     # 唯一 Pass：清屏 + 不透明 forward
├── RenderScene           # 帧期 draw 列表（可极简）
├── RenderMesh            # vb/ib + layout
├── RenderMaterial        # ShaderResource + 绑定缓存
└── AYRenderer            # 入口：持有 Pool + Adapter + Pipeline
```

### 3.2 完整模块（R3+ 逐步引入）

```
RenderPipeline
├── PassManager
├── ForwardOpaquePass / TransparentPass   # R3+
├── ShadowPass / GBufferPass / ...        # R5+ 延后
DrawListBuilder                           # R3：按 shader/material 分组
RenderResourceManager                     # R2
CameraManager / LightManager              # R3
```

### 3.3 每帧流程（R1）

```
beginFrame()
  ForwardOpaquePass::execute()
    setViewRect / clear
    for each RenderItem in scene:
      adapter.setTransform(item.world)
      adapter.setVertexBuffer / setIndexBuffer
      bind material uniforms/textures
      material.shader().submit({viewId, state})
endFrame()  → bgfx::frame()
```

R3+ 再扩展为多 Pass 循环。

---

## 4. BGFXAdapter

### 4.1 职责边界

| 负责 | 不负责 |
|---|---|
| `bgfx::init` / `shutdown` / `frame` | shader 编译、program 创建 |
| view rect / clear / debug text | uniform 名解析 |
| `setVertexBuffer` / `setIndexBuffer` | `bgfx::createProgram` |
| `setTransform` | `bgfx::createUniform`（由 AYShader 在 acquire 时完成） |
| 创建/销毁 VB / IB / **GPU Texture** | |

### 4.2 接口（R1）

```cpp
namespace ayt::renderer {

struct BGFXInitParams {
    void*    nativeWindowHandle = nullptr;  // 来自 AYDevice
    uint32_t width  = 1280;
    uint32_t height = 720;
    bgfx::RendererType::Enum backend = bgfx::RendererType::Count;  // Auto
};

class BGFXAdapter {
public:
    bool initialize(const BGFXInitParams& params);
    void shutdown();

    void beginFrame();
    void endFrame();

    void setViewRect(uint8_t viewId, int x, int y, int w, int h);
    void setViewClear(uint8_t viewId, uint16_t flags, uint32_t rgba,
                      float depth = 1.0f, uint8_t stencil = 0);

    void setTransform(const ayt::math::Float4x4& world);
    void setVertexBuffer(bgfx::VertexBufferHandle vb, uint32_t start = 0,
                         uint32_t count = UINT32_MAX);
    void setIndexBuffer(bgfx::IndexBufferHandle ib, uint32_t start = 0,
                        uint32_t count = UINT32_MAX);

    // 资源创建（R1）
    bgfx::VertexBufferHandle createVertexBuffer(const void* data, uint32_t size,
                                                const bgfx::VertexLayout& layout);
    bgfx::IndexBufferHandle  createIndexBuffer(const void* data, uint32_t size);
    bgfx::TextureHandle      createTexture2D(/* ... */);

    void destroy(bgfx::VertexBufferHandle h);
    void destroy(bgfx::IndexBufferHandle h);
    void destroy(bgfx::TextureHandle h);

    // 数学类型 overload 的 setUniform 已删除 — uniform 走 ShaderResource
};

} // namespace ayt::renderer
```

> **与旧 design 的差异**：旧版 BGFXAdapter 含 `setUniform(handle, FVector3)` 与 `createProgram(vs, fs)`；现 **uniform/program 全部交给 AYShader**，Adapter 只处理几何与帧。

### 4.3 初始化与 AYDevice

```cpp
bool AYRenderer::initialize(const RendererSettings& settings) {
    auto* window = device->window();
    BGFXInitParams p;
    p.nativeWindowHandle = window->getNativeHandle();
    p.width  = window->getWidth();
    p.height = window->getHeight();
    p.backend = settings.backend;
    return _adapter.initialize(p);
}
```

窗口所有权在 **AYDevice**；Renderer 不创建 SDL 窗口。

---

## 5. RenderMaterial

### 5.1 职责

- 持有 **`ShaderResource`**（来自 pool.acquire）
- 缓存常用 **`BindingId`**（避免每帧字符串查找）
- 持有 **property 默认值**（与 Phoskia property 对应）
- **不**持有 `bgfx::ProgramHandle`

### 5.2 接口（R2 完整；R1 可极简）

```cpp
class RenderMaterial {
public:
    bool loadFromPhoskiaFile(ShaderResourcePool& pool, const std::string& path);
    bool loadFromPhoskiaSource(ShaderResourcePool& pool, const std::string& src,
                               const std::string& cacheKey = "");

    shader::ShaderResource& shader() { return _shader; }
    const shader::ShaderResource& shader() const { return _shader; }

    // 按 Phoskia 名字设置（内部查 BindingId）
    void setPropertyFloat(const std::string& name, float v);
    void setPropertyVec3(const std::string& name, const ayt::math::FVector3& v);
    void setTexture(const std::string& name, bgfx::TextureHandle tex);

    // 将 property + 外部 override 写入 shader pending buffer
    void flushBindings();

private:
    shader::ShaderResource _shader;
    std::unordered_map<std::string, shader::BindingId> _uniformBindings;
    std::unordered_map<std::string, shader::BindingId> _textureBindings;
    // property 默认值 ...
};
```

### 5.3 与 aymat 资源的关系（R2+）

```
aymat (数据)                    RenderMaterial (运行时)
┌─────────────────────┐        ┌──────────────────────────┐
│ phoskiaPath: "..."  │ ──────►│ pool.acquire(source)     │
│ baseColor, metallic │        │ property → setUniform    │
│ albedoTex: resId    │        │ texture  → setTexture    │
└─────────────────────┘        └──────────────────────────┘
```

**R1**：Phoskia 路径硬编码或 JSON 配置即可。  
**延后**：旧 design 中的 `material_shader_mapping` SQL 与属性→shader 启发式规则；待 aymat 格式稳定后再做。

---

## 6. Draw 路径数据结构

### 6.1 RenderItem（R1）

```cpp
struct RenderItem {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  indexBuffer  = BGFX_INVALID_HANDLE;
    uint32_t indexCount = 0;

    ayt::math::Float4x4 worldTransform;

    RenderMaterial* material = nullptr;   // 非 owning

    uint64_t stateOverride = 0;           // 0 = Pass 默认
};
```

### 6.2 DrawItem / DrawGroup（R3+）

旧 design 中 `DrawItem` 含 `bgfx::ProgramHandle program` — **已删除**。分组键改为：

```cpp
struct DrawGroupKey {
    uint64_t shaderResourceId;   // ShaderResource::id()
    uint32_t materialInstanceId;
    // ...
};
```

`DrawListBuilder` 在 R3 引入，R1 直接遍历 `RenderScene` 线性提交。

---

## 7. RenderScene

帧期快照；与 ECS **松耦合**（由 RenderSystem 填充，不强制 ECS 类型进 Renderer 头文件）。

```cpp
class RenderScene {
public:
    void clear();

    void addRenderable(/* entity id, transform, mesh ref, material ref */);
    std::span<const RenderItem> items() const;

    // R3+
    void setActiveCamera(/* ... */);
    void addLight(/* ... */);
};
```

R1 可只有一个 `std::vector<RenderItem>`，无 Camera/Light。

---

## 8. RenderPipeline & Pass

### 8.1 RenderPass 基类

```cpp
class RenderPass {
public:
    virtual ~RenderPass() = default;
    virtual std::string_view name() const = 0;
    virtual void execute(RenderContext& ctx, const RenderScene& scene) = 0;
    void setEnabled(bool e) { _enabled = e; }
    bool isEnabled() const { return _enabled; }
protected:
    bool _enabled = true;
};
```

### 8.2 R1：ForwardOpaquePass

```cpp
class ForwardOpaquePass : public RenderPass {
public:
    std::string_view name() const override { return "ForwardOpaque"; }
    void execute(RenderContext& ctx, const RenderScene& scene) override;
};
```

### 8.3 当前 Pass 管线（R5 已落地）

| Pass | 当前状态 |
|---|---|
| ShadowPass | 已接入 Forward/Deferred，支持关键光和每光源 atlas 数据 |
| GBufferPass | 已落地四颜色 MRT + D24S8，契约 v3 已冻结 |
| LightingPass | 已落地全屏 Deferred 光照；shadow atlas、完整多光 BRDF、采样变体与 HDR 链路已完成第二轮收敛 |
| TransparentPass | 已接入双路径；Deferred 借用 LightingOutput 颜色与 GBuffer 深度组成缓存 FBO，共享多光/阴影契约，按 sortKey 与相机距离稳定排序并在输入失效时 fail-close |
| BloomExtract / BloomBlur | 已完成第二轮收敛：完整链门控、当前帧产出契约、RGBA16F、Karis 降采样、5-fetch/axis blur、曝光一致合成 |
| DepthHaze | 已完成第二轮收敛：全分辨率 HDR HazeColor、Coverage 背景语义、逐帧 fail-close、透明 PBR 雾化与显式 view 顺序；SSAO 已在 Lighting 环境光阶段完成，不在 Haze 内重复合成 |
| SSAO | 已完成审核与按序修复：RT3 coverage、TBN 旋转核、view-Z 比较、完整链门控、生命周期闭合，且只影响 Lighting 环境光 |
| PostProcess / UI | 已接入 |
| GBufferDebug | Deferred-only，view 250，默认关闭 |

当前 Forward 默认顺序：

```text
Shadow → ForwardOpaque → Forward2DOpaque → DepthHaze(no-op)
       → Transparent → BloomExtract → BloomBlur → PostProcess → UI
```

当前 Deferred opt-in 顺序：

```text
Shadow → Skybox → GBuffer → SSAO → Lighting → DepthHaze
       → Transparent → BloomExtract → BloomBlur
       → PostProcess → UI → GBufferDebug
```

### 8.4 UI 合批与绘制顺序

`UIRenderBackend` 先按 AYUI 产生的顺序记录整帧 `UiItem`，随后在 `flush()` 中选择提交顺序。默认的 `BatchMode::OverlapAware` 使用固定 96 项的前视窗口：后续兼容项只有在其与所有跨越项的绘制包围盒均不相交时才可前移。由此得到两个约束：

1. 任意重叠项的相对顺序不变，透明 UI、文字、阴影和边框仍遵守 painter order。
2. 不相交区域允许按纹理、state 或 SDF 参数聚拢，减少 submit 与状态切换。

兼容键如下：

| 类型 | 合批键 |
|---|---|
| Flat | bgfx state + texture |
| SDF | bgfx state + 完整 SDF 参数 |

调度器只生成逻辑索引，不改写原始 item 数组；实际 vertex/index 数据仍按既有路径构建和提交。窗口大小为常数，因此相对 item 数量的渐进复杂度保持线性。非有限包围盒、过大输入或内部校验失败会自动使用原始输入顺序。

Debug 构建还会对调度结果执行独立的不变量校验：结果必须是输入索引的完整排列，且每一对
相交 item 的 painter order 必须保持不变。该检查为 O(N²)，仅用于测试与开发构建；失败时
清空调度结果并回退到保守顺序，不进入 Release 热路径。

`BatchMode::OrderedRuns` 完整保留旧实现：只合并相邻且兼容的 item。该模式作为运行时兜底、排障开关和性能 A/B 基线存在，不需要维护第二套渲染后端。

### 8.5 Production UI Layer 与共享 RenderTargetPool

Renderer 在 adapter 初始化后创建 renderer-wide `RenderTargetPool`，并把同一实例注入 FrameGraph 与
`UIRenderBackend`。这不是“UI 专用缓存池”和“Pass 专用缓存池”两套实现；物理 FBO 的创建、复用、
延迟回收、预算和 reset 生命周期只有一个 owner。shutdown 顺序固定为：消费者释放 lease → pool
shutdown → adapter shutdown；resize/MSAA 切换顺序固定为：FrameGraph 释放 → pool reset → bgfx reset。

池的 key 为 `(width, height, colorFormat, withDepth, sampleCount, pointSampled)`，只做精确匹配。lease 由 slot 与
generation 组成，release 后旧 handle 立即失效；物理目标默认 quarantine 两帧，避免 CPU 已结束但
bgfx 队列仍引用 attachment。256 MiB 预算只淘汰已空闲且越过 quarantine 的 LRU 目标，因此是
best-effort：leased/quarantined 资源可暂时超预算。当前明确拒绝 sampleCount != 1。

`UIRenderBackend` 实现完整 RenderTarget create/resize/release/bind/texture/blit 和 Layer
create/update/release/paint/composite/invalidate。Layer 使用 RGBA8 + depth/stencil backing target；
逻辑 bounds 的 min/max 分别按 DPI 向外 `floor/ceil` 到物理像素，二者之差决定物理尺寸；完整
backing composite 后裁回 logical bounds。这个约束使小数 origin/extent 的离屏像素中心仍与主
framebuffer 对齐，不能退回只对 logical width/height 做 `ceil`。offscreen paint 使用 view 26–249
（250 留给 GBufferDebug），主
composite 使用 view 255。
target 切换是 batch barrier：进入离屏前 flush，保存 canvas/clip/path-clip/opacity/blend 状态；结束
paint 后 flush 并恢复。由此 flat、SDF、text、nine-patch、vector fill/stroke 和 nested stencil clip
继续走同一条生产图元路径。

UI RenderTarget 申请 point-sampled backing，保证同物理尺寸 composite 保持 texel identity；FrameGraph
目标默认仍为 linear，并由 pool key 隔离。Layer paint 对 straight-alpha 输出使用 RGB
`SRC_ALPHA/INV_SRC_ALPHA`、alpha `ONE/INV_SRC_ALPHA`，使目标保存 premultiplied RGB 和正确 coverage；
composite 再使用 premultiplied-over，不能对 alpha 做第二次相乘。局部 damage replay 必须保持原图元
参数空间：纹理按 clip fraction 重映射 UV，四角渐变按原 bounds 双线性重映射颜色。
Color clear 的全量 view clear 与局部覆盖 clear 都先写入 `(rgb*alpha, alpha)`；Additive、Multiply、
Screen 保留各自 RGB 方程，但 alpha 独立使用 `ONE/INV_SRC_ALPHA` coverage source-over。直接使用 bgfx
Multiply/Screen convenience state 会把 RGB factor 复用于 alpha，破坏不透明隔离层，属于错误实现。

AYUI 的 root Production Layer 是 opt-in。首次、无范围 dirty、resize/DPI/reset 帧完整绘制主树；
显式 dirty rect 帧只 replay damage clip。Transparent/Color Layer 先用无混合覆盖写清除 damage，
保留区域外像素；Preserve Layer 跳过清除。clean 帧只 composite 一次；overlay 与 drag visual 随后
即时绘制。capability、target 或 paint 失败时 AYUI 同帧回退原始路径。当前每帧最多 224 次
offscreen paint，第 225 次确定失败，下一帧重新从 view 26 分配。该上限只描述 pass 数量，不承诺
UI 独占 224 个共享 framebuffer。池与 UI backend 均限定 renderer thread。Noop 测试锁定生命周期和
submit 形态；显式 D3D11 运行 36 次独立 capture：原 1.0×/1.5× 复杂控件路径，以及透明 Layer、
group/nested opacity、Additive/Multiply/Screen 隔离组、resize、动态 DPI、device/MSAA reset lease
恢复和 Transparent/Color/Preserve 局部 clear。clean reuse、isolated blend、Preserve 字节精确；
通常最多差 1 LSB，RGBA8 group opacity 因离屏与最终合成两次量化最多差 2 LSB。MSAA 后 Layer 仍是
1× sample，因此生命周期门禁比较 recovered Layer 与 reset 后 fresh Layer，不拿 multisampled immediate
边缘作为错误参考。D3D12、OpenGL、Vulkan 同矩阵仍需补齐。

---

## 9. AYRenderer 主类

```cpp
namespace ayt::renderer {

struct RendererSettings {
    bgfx::RendererType::Enum backend = bgfx::RendererType::Count;
    bool enableDebugText = false;
    // R5+：enableShadows, enableDeferred, ...
};

class AYRenderer {
public:
    explicit AYRenderer(ayt::device::DeviceManager* device);
    ~AYRenderer();

    bool initialize(const RendererSettings& settings);
    void shutdown();

    void beginFrame();
    void renderFrame(const RenderScene& scene);
    void endFrame();

    BGFXAdapter& adapter() { return _adapter; }
    shader::ShaderResourcePool& shaderPool() { return _shaderPool; }
    RenderPipeline& pipeline() { return *_pipeline; }

    // R4
    void pollShaderHotReload() { _shaderPool.pollHotReload(); }

private:
    ayt::device::DeviceManager* _device = nullptr;
    BGFXAdapter _adapter;
    shader::ShaderResourcePool _shaderPool;
    std::unique_ptr<RenderPipeline> _pipeline;
};

} // namespace ayt::renderer
```

> **与旧 design 的差异**：不再内嵌 `AYShader m_shaderSystem` 或 `ShaderProgram*`；**`ShaderResourcePool` 为唯一 shader 运行时入口**。需要 AST 级编译时用 `phoskia::Compiler`，但 product 路径仍 `pool.acquire`。

---

## 10. 与其他模块

### 10.1 AYShader

| Renderer 调用 | 时机 |
|---|---|
| `pool.setShadercExecutable` / `setPlatform` / `setGLSLProfile` | 初始化 |
| `pool.acquire(path \| src)` | 材质加载 |
| `shader.get*Binding` | 材质 load 后缓存 |
| `shader.setUniform` / `setTexture` / `submit` | 每 draw |
| `pool.pollHotReload` | 每帧或 debounce |
| `pool.shutdown` | `AYRenderer::shutdown` 中、**bgfx::shutdown 之前** |

生命期顺序：

```
AYRenderer::initialize  → bgfx::init
                        → pool 配置
AYRenderer::shutdown    → pool.shutdown()   // 释放 bgfx shader handles
                        → adapter.shutdown() → bgfx::shutdown
```

### 10.2 AYDevice

- 提供 `nativeWindowHandle`、`width`、`height`、resize 事件
- Renderer 订阅 resize → 更新 `bgfx::reset`

### 10.3 AYResource（R2+）

`RenderResourceManager` 从 AYResource 加载 aymesh / aytex，上传 GPU，返回 `RenderMesh` / `bgfx::TextureHandle`。

### 10.4 AYEntity / ECS（Engine 集成，2026-07 落地）

```
EntitySubSystem::update()
  → World::update() → RenderSystem::onStart / onUpdate
RenderSystem::buildRenderScene()
  → query<Transform, MeshComponent>
  → Renderer::loadMesh / loadMaterial
  → RenderScene::add(mesh, material, worldMatrix)

GameLoop::submitRenderCommands()
  → RendererSubSystem::renderFrame()
  → scene builder callback → Renderer::render(scene)
```

| 类 | 职责 |
|---|---|
| `RendererSubSystem` | GameLoop 子系统；持有 `Renderer`；注册 render callback |
| `RenderSystem` | ECS System；填充 `RenderScene` |
| `bootstrapModule()` | 静态库显式注册 Entity 子系统 + RenderSystem + 组件类型 |

**Bootstrap API**（`AYRenderer/RendererSubSystem.h`）：

```cpp
RendererSubSystem::setBootstrapWindow(hwnd, w, h);
RendererSubSystem::setBootstrapShaderDumpDirectory(dir);  // 可选，initialize 后生效
```

**Shutdown 顺序**（避免静态析构崩溃）：

1. `GameLoop::shutdown()` → 逆序 `ISubSystem::shutdown()`
2. `RendererSubSystem::shutdown()`：清空 render callback → `Renderer::shutdown()`
3. `ShaderResourcePool` registry 使用 intentionally-leaked 堆存储，避免进程退出时 static 析构顺序问题

Renderer **不**依赖 ECS 头文件；只消费 `RenderScene`。

---

## 11. 多线程扩展（预留）

R1 不实现。接口预留：

```cpp
class IRenderCommandQueue {
public:
    virtual ~IRenderCommandQueue() = default;
    virtual void flush(BGFXAdapter& adapter, shader::ShaderResourcePool& pool) = 0;
};
```

---

## 12. 目录结构

### 12.1 R0–R1 最小树

```
AYRenderer/
├── README.md
├── design.md
├── CMakeLists.txt
├── include/AYRenderer/
│   ├── AYRenderer.h
│   ├── BGFXAdapter.h
│   ├── RenderPipeline.h
│   ├── RenderPass.h
│   ├── ForwardOpaquePass.h
│   ├── RenderContext.h
│   ├── AYRenderer/RenderScene.h
│   ├── RenderAYResource/AYResource/assetsImpl/Mesh.h
│   ├── RenderAYResource/AYResource/assetsImpl/Material.h
│   └── RendererSettings.h
├── src/
│   ├── AYRenderer.cpp
│   ├── BGFXAdapter.cpp
│   ├── RenderPipeline.cpp
│   ├── ForwardOpaquePass.cpp
│   ├── RenderScene.cpp
│   ├── RenderMesh.cpp
│   └── RenderMaterial.cpp
└── unittest/                    # R1 末
    ├── CMakeLists.txt
    └── Test_ForwardOpaque.cpp   # headless 或 demo window
```

### 12.2 R3+ 扩展

```
include/AYRenderer/
├── Draw/DrawListBuilder.h
├── src/detail/RenderResourceManager.h
├── Entity/CameraManager.h
└── Passes/ShadowPass.h ...
```

---

## 13. 实现路线图

### Phase R0 — 设计对齐

- [x] 修订 `design.md` 对齐 AYShader Phase 4
- [x] 添加 `README.md` 状态表
- [x] `CMakeLists.txt`

### Phase R1 — 最小上屏

- [x] `BGFXAdapter`：init / frame / view / vb / ib / transform
- [x] `ShaderResourcePool` wiring（shaderc 路径、platform 430）
- [x] `RenderMaterial`：`pool.acquire` + texture / property
- [x] `ForwardOpaquePass`：mesh + Phoskia material
- [x] 验证 draw 顺序 §2.5
- [x] 测试 + Demo

**验收**：屏幕出现 lit mesh；公开头文件无 `bgfx::ProgramHandle`。

### Phase R2 — 资源管理

- [x] `RenderResourceManager`：mesh/texture 缓存
- [x] AYResource bridge：aymat / aymesh / aytex
- [x] `SimpleLit` 级 material 跑通

### Phase R3 — 相机与光照

- [x] `setMainCameraLookAtPerspective`：view/proj → uniform
- [x] 方向光 frame uniform
- [x] `TransparentPass`

### Phase R4 — 工具链

- [x] `pollHotReload` / `pollShaderHotReload`
- [x] debug overlay（FPS / draw stats）
- [x] `captureScreenshot`

### Phase Engine — GameLoop + ECS（2026-07）

- [x] `RendererSubSystem` + `REGISTER_SUBSYSTEM`
- [x] `RenderSystem` + `bootstrapModule()` 引导
- [x] `AYEngineIntegration_Demo`：旋转 ECS 立方体 + overlay
- [x] 单线程 render callback（`setRenderThreadEnabled(false)`）
- [x] `UIRenderBackend` overlap-aware 合批 + `OrderedRuns` 兜底
- [x] Production UI Layer + renderer-wide RenderTargetPool（FrameGraph/UI 共享）

### Phase R5+ — Deferred 与高级 Pass

- [x] Shadow / GBuffer / Lighting / Transparent / PostProcess
- [x] Bloom / DepthHaze / SSAO / Skybox / GBufferDebug
- [x] GBuffer v3：帧有效性、资源所有权、逆转置法线、StandardLit/Unlit、AO/Model/Coverage 分离
- [ ] 真实 D3D11/12 GPU capture：MRT 值、Unlit/Cutout/Skinning/非均匀缩放与带宽
- [x] LightingPass 第一轮静态审核：架构、数据流、光照方程、阴影、生命周期与失败契约
- [x] LightingPass 第二轮收敛：每槽独立 Shadow view/VP、有效灯数与有限值、8 灯完整 BRDF、阴影采样变体、tile 安全采样、RGBA16F 中间链路
- [ ] LightingPass 真实 D3D11/12 GPU 验证；Point 全向阴影与 Spot 透视锥体阴影另立能力项
- [x] TransparentPass 第一轮静态审核：双路径目标、排序、深度/混合状态、光照与阴影契约、资源生命周期
- [x] TransparentPass 第二轮收敛：Deferred 借用附件、输入 fail-close、相机距离稳定排序、正确 alpha 语义、共享 8 灯/atlas/IBL 上传与 FBO 缓存失效
- [ ] TransparentPass 真实 D3D11/12 GPU capture：叠层玻璃、straight/premultiplied/additive、多光 atlas、resize/MSAA 缓存失效
- [x] BloomExtract/BloomBlur 第一轮静态审核：链路门控、数据流、HDR/曝光、失败路径、shader 质量与测试有效性
- [x] BloomExtract/BloomBlur 第二轮收敛：完整链零分配门控、逐帧产出 latch、参数清洗、HDR Karis 降采样、5-fetch/axis blur、Final fail-close
- [ ] Bloom 真实 D3D11/12 GPU capture：阈值/knee、曝光一致性、resize、格式 fallback、边缘采样与高亮稳定性
- [x] DepthHaze 第一轮静态审核：资源所有权、深度/背景语义、SSAO/透明/Bloom 顺序、失败路径与测试真实性
- [x] DepthHaze 第二轮收敛：全分辨率 HazeColor、Coverage 背景策略、AO-before-fog、逐帧 latch、透明 PBR 雾化与参数清洗
- [ ] DepthHaze 真实 D3D11/12 GPU capture：天空/地平线、遮挡边缘、透明叠层、additive、resize 与 HDR 格式 fallback
- [ ] 场景渲染 `DrawListBuilder` 合批（UI 合批已独立完成）
- [ ] Command Queue
- [ ] `material_shader_mapping` 数据库
- [ ] 从 AliyatRenderer 迁移 2D/UI/Skybox（**单独评估**；旧栈为 OpenGL，非直接移植）

---

## 14. 测试与生命周期约定（2026-07-20 补）

本节锁定 **测试 / CI 上不可忽略的两条 invariant**。两处都是真实坑,后人不要当偶发神 bug 重新踩。

### 14.1 Sticky Noop 后端 + 进程级 refcount

- `BGFXAdapter` 进程级 ref-count:同进程里首次 `initialize(Backend::Noop)` 锁 Noop,后续任何 `initialize(Backend::* | Auto)` 都返回 invalid(由 `57908fd` 一类修复引入,**未证明修干净**)。
- 一旦 Noop 锁上,**该进程任何后续 Renderer 都走 Noop**,即使显式要 Direct3D11 也无效。
- 这是 sticky 而非可恢复:若想真 GPU 必须 **新开进程**(独立 test exe / 新 fixture child)。
- **CI 落地:** `AYRenderer_Test` 永远 Noop(锁 sticky);真 GPU 跑窗口 Demo(`AYRenderer_Demo` / `AYEngineIntegration_Demo`);不要试图在 Noop 锁定的进程里切到真 backend。

### 14.2 Shaderc + 多 Renderer 实例

- `Test_RenderResources::textured_material_draw_one_frame` 创建 **第二个** Renderer → `createMaterialFromPhoskia` 触发 shaderc 子进程。
- 子进程路径上的 SIGSEGV / exit 139 bisect 多次稳定出现,与 Shadow 改动无强相关(见 `docs/execution-plan.md` §5)。
- 多 Renderer / 多 shaderc 子进程的组合在 Noop + Windows 子进程 pipe 上仍有 flaky 残余。修 ABI / 加 Pass / 切 FrameContext 签名时若撞上,**先跑 §5.4 隔离实验 ≥3 次**确认是否新引入,不要默认归 pre-existing rot 草率合并。

### 14.3 测试锚点

每次 PR 必跑:

- `Test_LightingCamera`(`forward_opaque_draw_one_frame` 类的现网回归锚)
- `Test_RenderResources::textured_material_draw_one_frame`(双 Renderer + shaderc)
- `Test_PostProcess_R5Plus_*`(R5.1 wire + P2 改动)
- `Test_ShadowPass`(cut-1 + 未来 cut-2)
- `Test_UIPass_AI1`(composite + UI view 255 锁)
- `Test_RenderTargetPool`(generation、quarantine、精确键复用、预算淘汰、reset)
- `Test_UILayerRenderTarget`(复杂图元/path clip、局部 damage、clean 单 composite、224 次离屏 pass
  溢出/跨帧恢复、resize/MSAA reset 重绘)

跑法:同一 commit **连续 3 次**全量 `AYRenderer_Test`,记录 PASS/FAIL;**3 次不全绿**则按 `docs/execution-plan.md` §5 处理,不许合并带赌的 ABI 变更。

2026-08-29 Production UI Layer/RenderTargetPool 阶段的 Windows Debug Noop 全量基线为
`3284 / 3284`。该结果验证 API、生命周期、view/submit 契约；D3D11 图像矩阵已补充像素正确性，
但仍不替代其余后端和 RenderDoc capture。

### 14.4 MSVC 增量对象与内部 ABI

- 修改 Pass 类布局、显式/隐式构造函数或 `PassExecContext` 尾字段后，如果出现随机 bool、内存断言、重复构造符号或测试进程挂住，先怀疑旧 `.obj` 与新头文件布局混用。
- 本轮 DepthHaze 曾由旧测试对象携带旧的隐式构造函数，导致 `_producedThisFrame` 初值随机；加入显式构造后，链接器进一步报告同一构造函数重复定义，最终通过清理并完整重建 `AYRenderer_Test` 目标消除。
- 此类问题不得靠放宽断言或增加运行时容错掩盖。先终止精确的残留测试进程，再对目标做干净重建；调试测试保持串行，避免多个断言窗口与 exe 文件锁互相干扰。

---

## 14. 与旧版 design 的主要变更摘要

| 旧设计 | 新设计 |
|---|---|
| `AYShader` + `ShaderProgram*` | `ShaderResourcePool` + `ShaderResource` |
| `DrawItem.program = bgfx::ProgramHandle` | 仅 `RenderMaterial::shader()` |
| `BGFXAdapter::createProgram` | 删除；`pool.acquire` |
| `BGFXAdapter::setUniform(bgfx::UniformHandle, ...)` | 删除；`ShaderResource::setUniform(BindingId, ...)` |
| Phase 1 含 GBuffer + Shadow | R1 仅 ForwardOpaque |
| `material_shader_mapping` SQL | R2+ 显式 phoskia 路径；SQL 延后 |
| Renderer 内嵌 `AYShader` 对象 | 持有 `ShaderResourcePool`；`Compiler` 按需局部使用 |

---

## 15. 参考

- [AYShader README](../AYShader/README.md)
- [AYShader design §8.5](../AYShader/design.md)
- [AYDevice design](../AYDevice/design.md)
- [bgfx API](https://bkaradzic.github.io/bgfx/)
- bgfx examples：`01-cubes`（R1 几何参考）

---

## 16. 变更记录

### 2026-08-29 — Production UI Layer 与共享 RenderTargetPool

- Renderer 新增唯一的 renderer-wide RenderTargetPool；FrameGraph 和 UIRenderBackend 共用，按精确
  descriptor 复用 generation lease，并提供两帧 quarantine、LRU best-effort 预算、reset/shutdown。
- FrameGraph 的 owned target 从直接创建/销毁迁移为 pool lease，standalone 测试仍可使用内部 owned pool。
- UIRenderBackend 完成 RenderTarget 和 Layer 全生命周期、纹理/blit、透明/color/preserve clear、
  offscreen view 26–249、主 view 255、target transition batch barrier 与状态恢复。
- AYUI root 主树可 opt-in retained pixel layer；clean frame 单 composite，overlay/drag visual 即时叠加，
  显式 dirty rect 局部清除/replay，无范围 dirty、resize/device reset 全量重绘，能力或 paint 失败同帧回退。
- Noop 回归覆盖复杂 gradients/SDF/nine-patch/vector fill/stroke/nested path clip、pool 回收和
  partial damage、224 次离屏 pass 溢出恢复、resize/MSAA reset；当前 Windows Debug 全量
  `3284 / 3284`。
- UI 目标的 point/linear sampling 纳入 pool 精确键；保留四参 framebuffer 入口并增加五参重载，
  避免增量对象 ABI 断裂。Layer 改用 coverage-correct alpha 与 premultiplied composite，gradient clip
  按原 bounds 重映射颜色。
- Auto 与显式 D3D11 在 1.0×/1.5× 下完成 10 场景真实纹理矩阵：immediate 30、full Layer 31、
  clean Layer 1、partial Layer 11 draw calls；语义差异最多 1 LSB，clean reuse 字节精确。
- D3D11 图像门禁扩展为 36 次 capture，新增透明 Layer、group/nested opacity、三种高级 blend、
  resize/动态 DPI、device/MSAA reset lease recovery 与 Transparent/Color/Preserve partial clear。
  该矩阵定位并修复小数 Layer origin 像素中心错位、高级 blend alpha 方程错误和半透明 Color clear
  未 premultiply 三项真实 GPU 缺陷；D3D12/OpenGL/Vulkan 同矩阵仍待完成。

### 2026-08-28 — GBuffer v3 冻结，进入 LightingPass 审核

- GBuffer 附件和打包索引收敛到 `GBufferLayout`，颜色 MRT 保持 160 bpp。
- 增加 `producedThisFrame()` 消费契约，防止冷启动/缩放/失败帧误读旧附件。
- StandardLit/Unlit 通过 RT2.a 与 AO 打包，RT3.a 独立保留 Geometry Coverage。
- 保留 RT2 WorldPosition；在跨后端 GPU 验证前不启用 Depth 重建。
- 暂不新增 Velocity RT；待 TAA/运动模糊成为真实消费者后再评估 32 bpp 增量。

### 2026-08-28 — LightingPass 第一轮审核

- 审核时结构是一个 view 8 的全屏光照 Pass：消费四个 GBuffer 颜色附件、Skybox/环境立方体、`SceneLights` 与 `ShadowPass`，当时输出独立 `RGBA8` LightingOutput；后续第二轮已升级为 `RGBA16F`，2026-08-29 又将 SSAO 调整为 Lighting 的前置 ambient 输入。
- 正向项：`GBufferPass::producedThisFrame()` 输入门禁、Lighting 自身逐帧产出门禁、冷启动/缩放前置 FBO 准备和资源销毁路径已经闭合。
- 阻断项：每光源 shadow atlas 复用同一 bgfx View 修改 view transform/scissor，且上传给 Lighting 的数组保存的是 View 而非 ViewProjection；Spot caster 还读取了 `Light::direction` 而不是 `spotDirection`。
- 高优先级项：多光源 PBR 只有槽 0 计算镜面项；阴影 shader 每像素静态执行 81 次采样；atlas 投影范围与 PCF tile 边界不严；未使用槽位会计算等边界 `smoothstep`；LightingOutput 为 LDR，曝光/tonemap 前已截断高光。
- 验证现状：B5/B5.5/B7 三组 220/220 断言通过，但本次运行未配置 shaderc，测试允许 program acquire 失败，且大多只验证字符串/契约；不能替代 D3D11/12 实机编译与 GPU capture。

### 2026-08-28 — LightingPass 第二轮按序修复

- Shadow atlas 生产端使用 view 1 整图清理、槽位独立 view 18–25 按 tile viewport/scissor 写入，并通过每帧 view order 保证其先于 resolve 与 Lighting；每槽分别保存 View、Projection 和 ViewProjection，Spot caster 使用 `spotDirection`。
- Shadow 只在当前帧实际生产并 resolve 后可采样；现代 `SceneLights` 无受支持 caster 时不回退旧关键光，legacy 单光路径统一发布为槽 0 全图采样契约。
- Lighting 上传精确 `activeLightCount`，未使用灯槽写入有限中性值，输入 light 参数做非有限值和范围清洗；8 个灯槽均执行完整 Cook-Torrance GGX，而非仅槽 0 有镜面项。
- Shader 按 shadow caster 数量与 PCF 开关生成变体：无阴影 0 次 shadow sample，硬阴影每槽 1 次，PCF 每槽 9 次；删除重复 legacy 关键光采样。
- Atlas 投影增加 clip.w、局部 UV 与参考深度门禁，PCF 中心限制在 tile 内侧 1.5 texel，避免跨 tile 污染；采样 texel 使用实际 4096 atlas 尺寸。
- LightingOutput、BloomBright、BloomBlurA/B 与 HazeColor 统一为共享 `RGBA16F` HDR 中间格式，最终 tone map/gamma 仍由 PostProcess 执行。
- 新增 `AYRenderer_LightingAuditRound2` 回归组，覆盖 HDR 格式、view 保留区、采样次数、无阴影公共计算裁剪和四类实际 Phoskia 变体 IR；定向 Lighting/Skybox 旧缓存键断言已同步。
- MSVC Debug 构建通过；`AYRenderer_Test` 全量 2936/2936 通过。该结果仍不替代真实 D3D11/12 shader 编译与 RenderDoc capture。
- 尚未关闭的生产门禁：真实 D3D11/12 shader 编译与 RenderDoc capture；Point 光全向阴影仍不支持，Spot caster 暂用 scene-fit 正交投影而非透视锥体。

### 2026-08-28 — TransparentPass 第二轮按序修复

- Forward 路径继续写入 scene FBO；Deferred 路径在 view 9 借用 LightingOutput 颜色附件和 GBuffer 深度附件组成缓存 FBO，透明对象只读深度、不写深度。GBuffer/Lighting 任一未在当前帧有效生产时直接 fail-close，不再退化到错误目标。
- 排序契约固定为显式 `sortKey` 降序优先、相机距离平方降序次优先、完全相等时保持输入顺序，并使用 `Sequential` view mode 保证 CPU 提交顺序不被重排。
- 抽取共享 `SceneLighting` 打包/上传路径供 Lighting 与 Transparent 使用；内建透明 PBR 支持 8 个 Directional/Point/Spot 灯完整 Cook-Torrance BRDF、caster-first 每光源 shadow atlas、tile 安全采样与环境立方体 IBL。
- 修正混合状态：straight 与 premultiplied alpha 均使用独立 alpha 通道 `ONE / INV_SRC_ALPHA`，additive 通过 `ZERO / ONE` 保留目标 alpha；所有透明模式统一 `LEQUAL` 且禁用深度写入。
- Transparent 借用 FBO 按颜色/深度句柄缓存，并在管线重配、resize、MSAA 修改与 shutdown 前主动销毁，避免持有已失效的生产者附件。
- 内建 PBR 已通过 Phoskia IR、Linux GLSL 430 与 Windows s_5_0 编译检查；新增/更新 Transparent、PBR 与共享灯光测试，相关回归均通过，手动使用当前 Ninja/MSVC 参数链接的 `AYRenderer_Test` 全量为 2987/2987。
- 尚未关闭的生产门禁：真实 D3D11/12 RenderDoc capture；传统 alpha 仍依赖排序，不提供 OIT；透明物体不写 GBuffer，因此 SSAO、轮廓和其它只消费 opaque GBuffer 的效果仍按 opaque 深度语义工作；DepthHaze 对内建透明 PBR 单独按 fragment world position 处理，自定义透明 shader 需要声明可选 haze uniforms；Point 全向阴影和 Spot 透视锥体阴影沿用 LightingPass 的后续能力项。

### 2026-08-28 — BloomExtract/BloomBlur 两轮审核与按序修复

- 架构固定为三个 FrameGraph 半分辨率瞬态目标：`BloomBright`、`BloomBlurA`、`BloomBlurB`。Pass 自身仅持有全屏几何、shader 和临时 attachment handle；A/B 的生命周期重叠，禁止物理 alias。view 顺序为 Extract 10、BlurH 11、BlurV 12，PostProcess 仍在 15。
- 数据流为 `Forward sceneFbo / Deferred LightingOutput → BloomExtract → BloomBright → BlurH → BloomBlurA → BlurV → BloomBlurB → PostProcess`。Forward scene FBO 与 Deferred LightingOutput 均优先申请共享 `RGBA16F` HDR 格式；不支持时由 BGFXAdapter 按候选格式安全降级。
- Extract 在 scene-linear HDR 上执行中心加四角共 5 次采样的 Karis 权重降采样，再以 `threshold` 与 fractional `softKnee` 提取高亮；Blur 使用线性过滤折叠的高斯核，每轴 5 fetch，替代原先每轴 9 fetch。
- Bloom 被视为单一完整能力：strength 必须为有限正数，且 Extract/Blur 均存在并启用，才声明三个 FG 资源；任一阶段缺失或禁用时零分配。strength、threshold、soft-knee 对负数、NaN、Inf 做统一清洗。
- Extract 与 Blur 每帧先清除 `producedThisFrame`，仅在真实 submit 后置位。Final 只有在 Blur 本帧生产且 `BloomSource`/attachment 均有效时才保留非零 strength；fallback sampler 仍绑定合法纹理，但 strength 强制为 0，避免旧帧采样和 `raw + scene * strength` 双亮。
- Bloom 与 scene color 在曝光前保持同一线性空间，Final 主 shader 使用 `(scene + bloom * strength) * exposure` 的等价组合后再 tone map/gamma；独立最小 fallback 只做 sceneColor blit，不依赖 bloom、曝光或 tone-map 语法。
- 测试直接检查生产 shader 字符串，不再维护易漂移的测试镜像；覆盖完整链 truth table、逐帧 fail-close、参数边界、HDR 格式、采样数/offset、binding 与曝光合成。精确生产源已手动通过 Phoskia IR、Linux GLSL 430 和 Windows s_5_0 检查；稳定全量 `AYRenderer_Test` 为 3022/3022。
- 可选后续优化按收益排序：先做真实 D3D11/12 capture 和阈值/曝光标定；需要更宽光晕时升级为多级 downsample/upsample 金字塔；再考虑 quarter-res 质量档、lens dirt、anamorphic streak 或 temporal stabilization。现阶段不引入多级链，避免在没有 GPU 基线前增加 RT、带宽与调参维度。
- 仍未关闭的生产门禁：真实 GPU capture、边缘采样/resize 观测、RGBA16F fallback 后的视觉降级验证；当前为单层半分辨率 Bloom，不具备多尺度光晕。

### 2026-08-28 — DepthHaze 两轮审核与按序修复

- 架构收敛为 FrameGraph 所有的全分辨率 `RGBA16F HazeColor`；DepthHazePass 自身只持有全屏几何、shader/binding 和 `producedThisFrame` latch。原 `HazeHalf` 数值 ID 4 保留为源码兼容别名，新代码统一使用 `HazeColor`。
- Deferred 数据流固定为 `GBuffer → optional SSAOTexture → LightingOutput + RT2 WorldPosition + RT3 GeometryCoverage → HazeColor → Transparent → Bloom → PostProcess`。显式 bgfx view order 保持稳定 ID 的同时强制 `SSAO(14) → Lighting(8) → Haze(13) → Transparent(9) → Bloom(10–12) → PostProcess(15)`。
- RT3.a 明确表示 geometry coverage：有几何像素按相机到 world position 的指数距离雾计算；无几何背景按无穷远策略接收完整请求强度，不再把清屏 world position 误判为近距离表面。
- SSAO 只在本帧实际产出且 semantic/attachment 有效时由 Lighting 消费，并仅衰减 StandardLit 的环境/IBL 项；DepthHaze 与 PostProcess 均不再读取 SSAO，因此 direct、emissive、unlit 与雾散射不会被整屏乘暗。
- PostProcess 删除独立 `hazeTexture` 与二次 haze composite，直接把本帧有效的 HazeColor 提升为主 scene source；Bloom 从同一 HDR source 提取。DepthHaze shader、PostProcess 主/回退 shader 均直接通过 Phoskia 与 Linux GLSL 430 编译验证。
- Transparent 在 opaque haze 后写回同一 HazeColor 并借用 GBuffer depth；内建 PBR 用 fragment world position 计算透明雾。Alpha 路径注入雾色，Additive 路径只按透射率衰减，避免凭空增加能量；ForwardOpaque 每次清零可选 haze uniforms，防止共享 program 状态串帧。
- strength/density/color 统一清洗 NaN、Inf 与负值；Pass/producer/backend/viewport 任一缺失时不声明资源或不提交。GBuffer、Lighting、SSAO、Haze 与 Bloom 的逐帧 latch 在图构建前统一复位，消费者不再把“句柄仍有效”误当成“本帧数据有效”。
- MSVC Debug 干净构建通过；四组 DepthHaze 定向契约 131/131，通过真实 shader 编译与 view-order 检查后，`AYRenderer_Test` 在同一代码上连续三次 3052/3052 全绿，未再出现断言窗或挂起。
- 可选优化顺序：先以 D3D11/12 capture 标定天空、地平线、轮廓边缘和透明叠层；确认带宽成为瓶颈后再评估 half/quarter-res + 双边上采样；待 GBuffer 深度重建方案成熟后可移除 RT2 WorldPosition；只有确有体积光需求时再升级 froxel/temporal volumetric fog。
- 剩余限制：自定义透明 shader 若不声明可选 haze uniforms 将保持未雾化；当前是解析式距离雾，不支持局部体积、光束或时间积累；全分辨率 HazeColor 增加一轮 HDR 读写，仍需真实 GPU capture 定量验收。

### 2026-08-29 — SSAOPass 审核与按序修复

- 架构固定为 Deferred-only 的 `GBuffer → SSAO → Lighting`：SSAO view 14 读取 RT2 world position、RT1 encoded normal 与 RT3.a geometry coverage，写入 FrameGraph 所有的全分辨率 `RGBA8 SSAOTexture`；R 保存遮蔽率，A 保存中心 coverage。Lighting view 8 是唯一消费者。
- 修正 coverage 来源：不再把 RT2.a 的 material AO/model 打包值当作几何有效位；中心与每个 kernel tap 均读取 RT3.a，天空/越界采样 fail-close。
- 8-tap kernel 以 GBuffer normal 构造 TBN，并按像素哈希旋转；样本从近到远分布于法线半球。遮挡比较使用符号无关的 `abs(view-space Z)`，bias 为固定视空间单位，不再比较相机径向距离或按距离放大 bias。
- 参数入口和逐帧广播统一清洗：strength 限制到 `[0,1]`，radius/bias 拒绝负值与 NaN/Inf，逐帧 bias 再限制到 radius。完整 gate 要求 SSAO/GBuffer/Lighting 三个 Pass 均存在且启用、后端与 viewport 有效、strength/radius 非零；否则不声明 SSAOTexture。
- 合成所有权收敛到 Lighting：coverage-aware 五点过滤后，AO 只乘 StandardLit ambient/IBL；direct、emissive 与 Unlit 保持不变。DepthHaze/PostProcess 删除 SSAO sampler、uniform 与重复滤波，避免整屏乘暗和有雾/无雾路径不一致。
- `producedThisFrame` 在每帧和每次 execute 起始清零，仅真实 submit 后置位。管线重配 `pipeline.clear()` 前和 Renderer shutdown/adapter teardown 前都显式销毁 SSAO 全屏 VB/IB 与 program/binding，关闭重启后的 stale handle/ABI 断言窗口。
- 测试改为直接消费生产 shader：覆盖参数非有限值、完整链 truth table、RT3 coverage、TBN、view-Z、ambient-only 所有权、Haze/PP 无重复 SSAO、cache key、latch 与 teardown 顺序，并实际执行 Phoskia + Linux GLSL 430 编译；不再用 `CHECK(true)` 或测试内复制 gate 形成假绿。
- 修改 `PostProcessPass` 私有布局后的增量产物曾在 `FinalPPPass_S1c` 触发 `Stack around the variable 'pass' was corrupted`；同一配置完整 clean rebuild 后该套件 46/46 正常退出，确认是新旧 `.obj` 混用导致的 ABI 污染，不是 SSAO shader 越界。随后 SSAO 161/161、Lighting 32/32、DepthHaze 64/64 定向回归通过，最终 MSVC Debug 全量 `AYRenderer_Test` 为 3095/3095，未再出现断言窗或异常退出。
- 暂不实施的可选优化：half-resolution + depth/normal-aware bilateral upsample、`R8` 单通道目标、由 depth 重建 view position、蓝噪声/temporal accumulation 与 GTAO。先用 D3D11/12 capture 测量 SSAO 带宽、边缘 halo、噪声和参数尺度，再按收益选择，避免在无 GPU 基线时同时改变格式、分辨率和算法。

### 2026-08-29 — PostProcessPass 审核与正确性收敛

- PostProcess 固定为最终全屏 composite，不再拥有私有 FBO：输入由共享 `SceneColorPipeline` 选择 Forward `sceneFbo`、Deferred `LightingOutput` 或本帧有效的 `HazeColor`，输出直接写默认 backbuffer，随后由 UI view 255 合成。
- `SceneColorPipeline` 成为 Haze、Transparent、Bloom 与 PostProcess 的共同路由所有者；`PostProcessPass::selectSourceFbo` 只保留兼容转发，避免其它 Pass 依赖最终合成类的私有策略。
- Pass 本地生命周期仅包含 fullscreen VB/IB、ShaderResource 与 binding。Renderer 在 `applyPipelineDesc()` 的 `pipeline.clear()` 前以及 shutdown 的 shader pool/adapter teardown 前显式调用 `destroyResources()`，关闭重配和 shutdown→initialize 的 stale numeric handle 路径。
- 主 Shader 删除未消费的 `uTime` ABI。Exposure 在 `[0,64]`、Gamma 在 `[0.1,8]` 内清洗；NaN/Inf 使用中性默认值，并在公开 setter、FrameContext 广播和 Pass 上传三层 fail-safe。
- 原先与主 Shader 几乎同构的 fallback 改为只声明 `sceneColor` 的独立最小 blit；fallback 可见时仍每 120 帧尝试恢复主程序，两者都失败时也不会逐帧调用 shaderc。
- ForwardOpaque 在设置 scene clear 后显式 `touch(viewId)`，保证场景列表非空但没有有效 opaque submit 时仍写入本帧清屏色，PostProcess 不再采样上一帧 scene FBO。
- `PostProcessPass` 构造/析构改为 `.cpp` 唯一定义，避免 Visual Studio 增量构建从旧测试对象选择按历史类尺寸生成的 inline COMDAT；本轮已用链接器符号定位并定点重编相关 AYRenderer 测试对象，没有执行全引擎 clean。ForwardOpaque 的 initialized gate 同时前移到 `setViewMode` 之前，修复未初始化 bgfx 下的旧 R5Plus pipeline 挂起。
- 测试不再维护 PostProcess Shader 的本地镜像或用故意编译失败代替生产编译；主/回退源码直接从运行时代码导出，覆盖 binding、cache key、参数边界、共享路由、bounded retry、teardown 顺序与 Forward touch。
- PostProcess 定向回归全部通过：R51 Smoke 2/2、R51 62/62、FinalPP S1c 54/54、P0 13/13、F5 27/27、R5Plus 19/19、B6 SourceFbo 9/9，并通过相关 Bloom、DepthHaze、SSAO 与 AuditP5 回归。最初 3 个 UI Layer 容量断言把“每帧 224 个 offscreen pass”误写成“同时持有 225 个共享 framebuffer”；改为在同一有效 Layer 上隔离验证 view 分配并锁定 26/249/224 常量后，MSVC Debug 全量为 `3211 / 3211`。
- 暂不加入 dithering、精确 sRGB OETF、HDR swapchain 输出、额外 tone-map 算法或多级 bloom；先完成 D3D11/12 真机 Shader 编译与 RenderDoc capture，再按 banding、色彩管理和性能数据决定。

### 2026-07 — 引擎闭环（R4 + Engine）

- `RendererSubSystem`：GameLoop 子系统，单线程 `renderFrame` callback。
- `RenderSystem`（AYEntity）：ECS → `RenderScene` → `ForwardOpaquePass`。
- Demo：`AYEngineIntegration_Demo` 验证全链路。

### Bootstrap / Shutdown

| API | 说明 |
|-----|------|
| `setBootstrapWindow` | Win32 HWND + 分辨率（`initialize` 前） |
| `setBootstrapShaderDumpDirectory` | Phoskia 中间产物 dump（`initialize` 后写入 pool） |
| `RendererSubSystem::shutdown` | 先 `setRenderCallback({})`，再 `Renderer::shutdown()` |
| `GameLoop::shutdown` | Demo 退出前调用，逆序关闭子系统 |

### AYShader 协作修复

- `ShaderResourcePool::Impl` 的全局 pool registry 改为 **intentionally-leaked** 堆 map，
  修复进程退出时 `unregisterPool` 访问已析构 static map 的 AV（C0000005）。

### 依赖模块同步

- **AYEntity**：`bootstrapModule()`、`SparseSet` 指针语义（见 AYEntity `design.md` §15）。
- **AYCore**：`AYCore/CoreSerializer.h` 提升属性宏。
- **AYSerializer**：`SerializerFor<T,void>` 默认 reflect 路由（见 AYSerializer README §变更记录）。

### 下一步

PostProcess 正确性修复完成后，按 Pass 顺序进入 UIPass/最终合成边界审核；并行保留 GBuffer、Lighting、Transparent、Bloom、DepthHaze、SSAO 与 PostProcess 的真实 GPU capture 门禁。之后继续推进 Point/Spot 高级阴影、Command Queue 与 DrawListBuilder。
