# AYRenderer Design

> **文档状态**：2026-07 修订 — 对齐 AYShader Phase 4；**R0–R4 + Engine 集成已落地**（见 §13、§16）。
> **实现状态**：2026-07-20 复核，`include/` / `src/` / `unittest/` / `demo/` 已落地；默认管线 = `[ForwardOpaque, Transparent, PostProcess, UI]`（4 Pass）；ShadowPass 源码存在但**未 `addPass`**（cut-1 stub:identity light xform + depth-only FBO + no-fragment-program submit；见 `docs/execution-plan.md` §1.2 / §5）；GBuffer / Lighting 仍未实现，仅在本文档保留设计占位。
> **活动执行计划**：[`docs/execution-plan.md`](execution-plan.md)（P0–P6 队列、§5 segfault 约束、§5.4 隔离实验、附录 B/C/D 索引）。本文件是目标架构，与代码不一致时以代码与 execution-plan 为准。
> **关联文档**：[`AYShader/design.md` §8.5](../AYShader/design.md)（opaque handle contract）、[`AYShader/README.md`](../AYShader/README.md)。

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
| **先 Forward、后 Deferred** | R1–R3 只做前向最小闭环；GBuffer / 阴影 / 后处理标注延后 |
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

### 8.3 延后 Pass（R5+，不在 R1–R4 实现）

| Pass | 依赖 AYShader 能力 |
|---|---|
| ShadowPass | `texture2dshadow`（Phase 6） |
| GBufferPass | MRT（Phase 6） |
| LightingPass | deferred + 多 UBO |
| TransparentPass | blend state + 排序 |
| PostProcessPass | 全屏 triangle + storage image（Phase 5 延后） |
| UIPass | 与 AYUI 集成 |

默认 Pass 顺序（**完整管线目标**；R1 仅 `"ForwardOpaque"`）：

```cpp
static constexpr std::string_view kFullPipelineOrder[] = {
    "Shadow", "GBuffer", "Lighting", "ForwardOpaque",
    "Transparent", "PostProcess", "UI",
};
```

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
- [ ] 可选 `TransparentPass`（延后）

### Phase R4 — 工具链

- [x] `pollHotReload` / `pollShaderHotReload`
- [x] debug overlay（FPS / draw stats）
- [x] `captureScreenshot`

### Phase Engine — GameLoop + ECS（2026-07）

- [x] `RendererSubSystem` + `REGISTER_SUBSYSTEM`
- [x] `RenderSystem` + `bootstrapModule()` 引导
- [x] `AYEngineIntegration_Demo`：旋转 ECS 立方体 + overlay
- [x] 单线程 render callback（`setRenderThreadEnabled(false)`）

### Phase R5+ — 延后

- [ ] Shadow / GBuffer / PostProcess
- [ ] `DrawListBuilder` 合批
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
- `Test_UIPass_AI1`(composite + UI view 2 锁)

跑法:同一 commit **连续 3 次**全量 `AYRenderer_Test`,记录 PASS/FAIL;**3 次不全绿**则按 `docs/execution-plan.md` §5 处理,不许合并带赌的 ABI 变更。

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

## 16. 变更记录（2026-07）

### 引擎闭环（R4 + Engine）

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

### 下一步（R5+，不阻塞 AYUI）

Shadow / GBuffer / PostProcess / Command Queue / TransparentPass / DrawListBuilder。
