# AYRenderer

AYRenderer 是 AY Engine 的**渲染器子系统**：基于 bgfx，负责帧调度、视口、几何提交、场景数据收集，并通过 **AYShader `ShaderResource`** 绑定 Phoskia 材质（不自行编译 shader、不在材质路径上暴露 bgfx program/uniform handle）。

完整设计见 [`design.md`](design.md)。

---

## 状态

**R1 已落地** — 公开 API 不含 bgfx；`ForwardOpaquePass` + Noop 测试通过。

| Phase | 范围 | 状态 |
|---|---|---|
| R0 | design 对齐 AYShader + README + CMake | ✅ |
| R1 | `Renderer` pimpl + `ForwardOpaquePass` + unit cube + Phoskia material | ✅ |
| R1.5 | `VertexLayoutDesc` + `createMesh` | ✅ |
| R2a | `RenderResourceManager` + texture upload/bind (AYIO, no AYResource) | ✅ |
| R2b | AYResource bridge (aymat / aymesh / aytex) | ✅ |
| R3 | Camera + directional light frame uniforms | ✅ |
| R4 | hot-reload（`pool.pollHotReload`）+ 开发调试 overlay | ✅ |
| R4-a | hot-reload：`compileFromFile` watch + poll + 材质自动 refresh | ✅ |
| R4-b | debug overlay：FPS / draw 统计 / bgfx debug text | ✅ |
| R4-c | `captureScreenshot`：backbuffer PNG（窗口 + 真实 GPU backend） | ✅ |
| Engine | GameLoop + Entity `RenderSystem` + `RendererSubSystem` | ✅ |
| R5+ | PostProcess | ✅ Phoskia 程序 + **从 scene FBO attach0 采样** + 真 blit-back（PR-D 2026-07-20, commit `b66deb8`）；bloom/exposure/tonemap 真作用 |
| R5+ | Shadow | 🅪 cut-2 ✅ / cut-3 ❌ — depth-only FBO + **真 light-space ortho**（PR-F1' 2026-07-21, commit `502458b`,feat branch）已 ship；**PR-F2 (2026-07-21, commit `8a646ae`) ship** — FO/Transparent 通过 `PassExecContext::shadowPass` 拿 shadow producer 句柄、上传 `u_lightViewProj`、绑 `shadowMap`（bgfx D24S8 depth 平面 `.r` 通道手动比较）。demo 截图会有 hard-edge shadow（bias acne 还在,但屏幕可见）。新 `simple_lit_shadow.phoskia`（`out worldPos : position` varying + 手动 depth compare）也 ship。**PR-F3 (2026-07-21, commit `ea5019d`) ship** — ShadowPass 现在用真 Phoskia `shadow_caster.phoskia` 程序替代 `bgfx::ProgramHandle{BGFX_INVALID_HANDLE}` 裸 submit，双段 VS（`castSkinned` 属性 0=静态、1=skinned 走 `skinningMatrix(...)`），复用 `Skeleton` UBO 路径；新提 `tryUploadBonePalette` helper 同时供 FO 与 ShadowPass 调用，FO skinned draw path 字节不变；PassExecContext 不动（master "caster 状态在 ShadowPass 私有" 铁律），17 plumbing 测试 + 502/502 3 跑稳。**仍不默认挂 Shadow**（§5.4 E4 未跑）；host opt-in 路径: `pipeline.addPass(ShadowPass)` + `ctx.shadowPass = &shadow`。 |
| R5+ | GBuffer / Lighting / Command Queue | ❌ missing — 仅设计，无代码 |

---

## 当前定位（产品角度，2026-07-21 反映 PR-F3）

**适合当前能做的：** Editor / Demo 的前向场景 + UI 合成 + 资源加载 + 蒙皮（bone UBO 已 wire,在 FO 与 ShadowPass 两端均 ship）+ 真 bloom/exposure/tonemap post-process + **带 skinned caster 的方向光 shadow**（硬边,bias acne 可调）。

**还不能当完整渲染器当的：** 延迟渲染、复杂后处理（真 bloom chain / DOF / SSR）、产品级 PCF / VSM shadow 滤波。

### 唯一"差一步能 demo"的:**PR-F2** ✅ 已 ship(2026-07-21,commit `8a646ae`)

| 项 | 工作量 | 已 ship |
|---|---|---|
| FO 加 shadow 采样 + `bias` + 接入 F1' getter(Phoskia 手动 depth compare,见 `simple_lit_shadow.phoskia`)| 1 PR ≈200 行 | ✅ `PassExecContext::shadowPass` + `tryBindShadowSampler` helper |
| + shadow_caster 程序段(skinned 模型 bone UBO 走 shadow depth)| 加在 PR-F2 同 PR 即可 | ✅ ship `ea5019d` —— inline `kShadowCasterPhoskiaSource` + `shadow_caster.phoskia` demo 副本 + `tryUploadBonePalette` helper。ShadowPass 不再裸 `bgfx::INVALID program` 冒充 skinned。|
| Host opt-in path | — | host 调 `pipeline.addPass(ShadowPass)` + 派发前 `ctx.shadowPass = &shadow` 即可 |

PR-F2 ship 后 **demo 屏幕有 hard-edge shadow**(直视 bias 还可能 acne,可调 `shadowBias` property)。

### 表中★数字 **不等于**"要做几个 PR":仅是设计复杂度

下面这表是「**未来**某条线要做的所有事」的清单,**不代表**接下来的工作量。每行★是设计难度,不是承诺：

| 关卡 | 缺口 | 设计难度 | 说明 |
|------|------|---------|------|
| Light API 直连 (`addLight(...)`) | F1' 把 `RenderScene::Light` 隔离在 `AY_F1_DIAG_LIGHT` OFF；要 host 可调需开 flag+跑 §5.4 bisect 矩阵 | ★★ | flag 控制混编风险 |
| Cascade / 多光 atlas | 单 directional。多光需 atlas 或 cascade | ★★★ | 推迟到 P5 之后 |
| PCF / VSM 滤波 | hard depth compare，锯齿明显 | ★★ | 推迟 |
| 真 Scene-AABB 紧贴 | F1' 用固定 50 单位 ortho 包围原点；超出 ±50 物体阴影 clamp | ★★ | 简单 `computeAABB()` |
| 多 RenderTarget + LightingPass（GBuffer） | 完全没规划 | ★★★★ | 不在 P0–P6 窗口 |

**简版路线：** S1 = **PR-F2 (1 PR,现在就差这 1 个)** = "单 directional 灯 + 一片 hard-edge shadow demo"；S2 = "中端 3A demo" 在 S1 之上展开。

---

## 引擎用法（无 bgfx）

```cpp
#include "AYRenderer.h"

ayt::render::Renderer renderer;
ayt::render::InitDesc init;
init.windowHandle = myWindow;   // 来自 AYDevice / SDL / Win32
init.width  = 1280;
init.height = 720;
init.backend = ayt::render::Backend::Auto;
init.enableDebugOverlay = true;  // FPS / draw stats HUD (bgfx debug text)

renderer.initialize(init);

ayt::render::MaterialHandle mat =
    renderer.createMaterialFromPhoskia(phoskiaSource);
renderer.setMaterialColor(mat, "baseColor", 1.f, 0.2f, 0.1f, 1.f);

ayt::render::MeshHandle mesh = renderer.createUnitCube();
// Or load cooked assets via AYResource (R2b):
// ayt::render::MeshHandle mesh = renderer.loadMesh("assets/models/cube.aymesh");

renderer.setDirectionalLight(ayt::math::FVector3(0.2f, -1.0f, -0.2f),
                             ayt::math::FVector3(1.0f, 0.95f, 0.85f));

ayt::render::RenderScene scene;
scene.add(mesh, mat);

renderer.beginFrame({});
renderer.render(scene);
renderer.endFrame();

// Optional: read stats without HUD
// const ayt::render::RenderFrameStats& stats = renderer.getFrameStats();
// renderer.setDebugOverlayEnabled(false);

// Screenshot (after render, before endFrame; window + real GPU backend)
// renderer.captureScreenshot("capture.png");
```

公开头文件：`AYRenderer.h`、`AYRenderer/RenderScene.h`、`AYRenderer/RenderTypes.h`、`AYRenderer/RendererSubSystem.h`、`AYRenderer/RendererRuntimeModule.h`。**不含** `<bgfx/bgfx.h>`。

---

## 引擎集成（GameLoop + ECS）

最小链路：

```
EntitySubSystem::update  →  World::update  →  RenderSystem（注册 scene builder）
GameLoop::submitRenderCommands  →  RendererSubSystem::renderFrame  →  Renderer::render
```

1. **Win32 窗口** — 启动前调用 `RendererSubSystem::setBootstrapWindow(hwnd, w, h)`。
2. **子系统** — 默认 Host 通过 `EntityRuntimeModule` + `RendererRuntimeModule` 显式装配；不再依赖静态 `REGISTER_SUBSYSTEM`。独立 Demo 可继续调用 `bootstrapModule()` 与 `RendererSubSystem::registerSubSystem()`。`RenderSystem` 把带 `Transform` + `MeshComponent` 的实体提交到 `RenderScene`。
3. **单线程渲染** — `RendererSubSystem::initialize` 会 `setRenderThreadEnabled(false)` 并在 `submitRenderCommands()` 中同步执行 render callback。

```cpp
#include "AYEntity.h"
#include "AYEntity/EntityModule.h"
#include "AYGameLoop.h"
#include "AYRenderer/RendererSubSystem.h"

HWND hwnd = /* create window */;
ayt::render::RendererSubSystem::setBootstrapWindow(hwnd, 1280, 720);

auto& loop = ayt::game::GameLoop::instance();
loop.setRenderThreadEnabled(false);

ayt::entity::bootstrapModule();   // 兼容路径：Entity + presentation systems
ayt::render::RendererSubSystem::registerSubSystem();

loop.run();   // pump Win32 messages in onUpdate()
loop.shutdown();
```

> 完整设计见 [`../AYEntity/design.md`](../AYEntity/design.md) §15。

**集成 Demo**（旋转 ECS 立方体 + debug overlay）：

```bat
cmake --build D:\Projects\out\build\x64-Debug --target AYEngineIntegration_Demo
D:\Projects\out\build\x64-Debug\AYRuntime\AYRenderer\demo\AYEngineIntegration_Demo.exe
```

---

## 构建与 Demo

**单元测试**（无窗口，bgfx Noop）：

```bat
cmake --build D:\Projects\out\build\x64-Debug --target AYRenderer_Test
D:\Projects\out\build\x64-Debug\AYRuntime\AYRenderer\unittest\AYRenderer_Test.exe
```

**窗口 Demo**（旋转立方体，验证 AYRenderer + AYShader 全链路）：

```bat
cmake --build D:\Projects\out\build\x64-Debug --target AYRenderer_Demo
D:\Projects\out\build\x64-Debug\AYRuntime\AYRenderer\demo\AYRenderer_Demo.exe
```

**原生 bgfx 对照 Demo**（绕过 AYShader / AYResource，顶点色 cube）：

若全链路 Demo 画面异常，先跑此对照以区分「bgfx/窗口问题」与「AYShader/材质桥接问题」：

```bat
cmake --build D:\Projects\out\build\x64-Debug --target AYRenderer_BgfxSanityDemo
D:\Projects\out\build\x64-Debug\AYRuntime\AYRenderer\demo\AYRenderer_BgfxSanityDemo.exe
```

- 预期：彩色旋转 cube（每面不同 ABGR 顶点色），背景深灰蓝
- 启动时用 vcpkg `bgfx[tools]` 提供的 shaderc 编译内置 `vs_color` / `fs_color`（bgfx include 由 CMake 包自动提供）
- **Esc** 或关闭窗口退出

| 结果 | 含义 |
|---|---|
| BgfxSanity 正常、Demo 全黑 | 问题在 AYShader 编译/绑定或 AYRenderer 材质路径 |
| 两者都黑 | 优先查 bgfx 初始化、GPU 驱动、shaderc 编译日志 |

- 1280×720 Win32 窗口，蓝色旋转 cube
- 左上角 **debug overlay**（FPS、draw 数、backend、分辨率）
- **F9** 保存 `{assetRoot}/screenshot.tga` 与 `screenshot.png`（stderr 会打印完整路径）
- **Esc** 或关闭窗口退出
- 需要 vcpkg `bgfx[tools]`；CMake 自动定位 shaderc，`common.sh` / `shaderlib.sh` 使用 AYShader 内受版本控制的 `shaderinclude/bgfx`

---

## Shadow / 调试 env 开关

Shadow 子系统所有"开关"都是 **运行时 env**,不动编译 flag,默认值保 production-safe:

| env | 默认 | 含义 |
|---|---|---|
| `AY_SHADOW_USE_MAP` | `0` (off) | **force-lit fallback**:Receiver 永远看到 fully-lit(0.20 底色 + ndotl × 0.65)。**任何 demo 验证 shadow 真可视必须 `set AY_SHADOW_USE_MAP=1` 否则屏幕只看到 force-lit**。bgfx `.r` channel SRV readback 路径未在所有 backend 验证完之前,默认 OFF 防 regression。 |
| `AY_SHADOW_DEBUG` | off | `1` 启用 receiver `shadowDebugVis`:fragment 用 shadowMap `.r` 灰度覆盖 lit,直观看 shadow map 长啥样。 |
| `AY_SHADOW_CASTER_SOLID` | off | `1` caster 写常量 0.5 到 color RT(忽略 z)── 验证 FBO / resolve / 路径 wired,不看真 depth。 |
| `AY_SHADOW_LOG` | `2` (Frame summary) | 0=Silent / 1=Caps / 2=Frame(默认)/ 3=Probe / 4=Verbose。Demo 默认 level 2:首 8 frame 打印 ShadowPass summary。 |

**SuzanneSkinnedDemo 验 shadow 真可视:**

```bat
set AY_SHADOW_USE_MAP=1
set AY_SHADOW_LOG=3
D:\Projects\out\build\x64-Debug\AYRuntime\AYRenderer\demo\AYSuzanneSkinned_Demo.exe
```

`AY_SHADOW_USE_MAP=1` 没设 = force-lit(看不到 shadow,但也不挂);设为 `1` 才会真显 shadow。

**Editor Play:** Editor 启动时把 `AY_SHADOW_USE_MAP` 注入到子进程 env,所以 Editor 默认就有 shadow 可视。Standalone demo 缺 env 就走 force-lit。

---

## UI 合批

`UIRenderBackend` 默认使用 `BatchMode::OverlapAware`。它不会对整帧 UI 做全局排序，而是在一个有界窗口内寻找兼容绘制；候选项只能跨过与自身绘制包围盒不相交的项，因此所有重叠元素仍保持原始 painter order。

- Flat UI 按纹理与 bgfx state 合批。
- SDF UI 按 state 与完整 SDF 参数合批。
- 搜索窗口固定为 96 项，调度成本随 UI item 数量线性增长，不会出现无界排序开销。
- 包围盒含非有限值或调度校验失败时，自动回退到原始顺序。
- `setBatchMode(BatchMode::OrderedRuns)` 可显式启用旧的“仅相邻兼容项合批”路径，便于兼容、排障和 A/B 对比。

专项基线场景中，默认模式把 23 次 UI submit 降至 6 次；旧路径仍由同一组测试锁定为 23 次。

---

## Production UI Layer 与 RenderTargetPool

`UIRenderBackend` 已启用 AYUI 的 RenderTarget/Layer capability。静态 UI 主树可以绘制到保留像素层；
clean 帧只在主 UI view 提交一次 composite，popup、modal、tooltip 和 drag visual 继续在其后即时提交。
离屏层完整复用现有 flat/SDF/text/nine-patch/vector-path 流程，包含 depth/stencil，因此复杂路径与嵌套
path clip 不需要维护第二套实现。create/update/paint 失败或 device reset 后，AYUI 会标脏并同帧回退/
重绘。显式 dirty rect 使用 damage clip 做局部 replay；Transparent/Color Layer 先以无混合覆盖写
清除受损区域，区域外像素保持不变，Preserve Layer 则跳过该清除。

Renderer 持有一个共享 `RenderTargetPool`，FrameGraph 的 Bloom/Haze/SSAO 等目标与 UI Layer 都从该池
取得 generation-checked lease。池按尺寸、格式、depth、sampleCount、采样方式精确复用，release 后默认隔离两帧，
空闲目标按 LRU 受 256 MiB 预算约束；resize、MSAA 切换和 device reset 会统一失效所有 lease。
FrameGraph 保持允许临时超预算的 soft acquire；UI RenderTarget/Layer 使用 strict acquire。UI 压力下
先清理 idle LRU，再撤销最久未 composite 的 Layer backing；逻辑 LayerHandle 保持有效并标脏，当前帧
无法重新取得 target 时 AYUI 走 immediate fallback，quarantine 结束后再复用/分配。当前限制为：

- 仅支持 1× sample 的池目标；FrameGraph 的 leased/quarantined 目标可能暂时超过 soft 预算，UI strict
  acquire 不会新增超预算 target。
- UI 离屏 paint 使用 view 26–249（250 保留给 GBufferDebug），每帧最多 224 次；第 225 次失败并由
  AYUI 同帧回退，下一帧从 view 26 恢复。该上限是 pass 调度容量，不是 UI 独占 framebuffer 数量。
- 池和 UI backend 都是 renderer-thread-only。
- UI RenderTarget 使用 point sampling，避免同尺寸 composite 对抗锯齿边缘和 texel 做线性重采样；
  FrameGraph 目标仍默认线性采样，pool key 保证两者不会误复用。
- OpenGL/Vulkan 等 RenderTarget 读取通过 bgfx caps 的 `originBottomLeft` 决定 V 方向；point-sampled
  glyph quad 吸附物理像素网格，避免默认 framebuffer 与 FBO 对半像素边界选择不同 coverage。
- Text shaping 以完整 grapheme 选择 primary/fallback face，罕见脚本按需发现并缓存系统字体；每个
  shaped font run 使用自己的 atlas，但继续进入同一 UI item batch。FreeType BGRA color glyph 保留
  固有颜色，普通 coverage glyph 仍由 TextStyle 着色。
- Layer backing 的物理 min/max 分别按 DPI 向外对齐，保留与主 framebuffer 相同的像素中心；合成完整
  backing 后裁回 logical bounds，支持小数 origin/extent 而不产生一像素接缝。
- Layer 内部按正确 coverage alpha 累积 straight-alpha 图元，最终以 premultiplied-over composite；
  半透明 Color clear 写入 premultiplied color，Additive/Multiply/Screen 的 alpha 独立按 source-over
  coverage 累积。damage clip 下纹理 UV 和四角渐变颜色都按原 bounds 重映射。
- Layer 支持 root 与 subtree cache；damage sidecar 最多保留 8 个区域，累计面积达到 70% 时 full repaint。
  `LayerCacheStats` 暴露 paint/composite/hit/repaint area、分配失败/降级及 pool allocation/reuse/eviction、
  live/idle/bytes/budget，便于生产 telemetry 和 Auto 策略调优。
- Noop 契约测试覆盖复杂绘制、多区域 damage、subtree Auto/Always、clean composite、strict budget/LRU
  降级、离屏 pass 溢出恢复与 reset 重绘；D3D11、D3D12、OpenGL、Vulkan 均通过同一 36-capture 的
  1.0×/1.5× full/clean/partial、透明/opacity/blend、resize/DPI、reset/MSAA lease 恢复和三种 clear
  mode 真实 GPU 图像矩阵。

Windows Debug 当前全量基线为 `3334 / 3334` 条断言通过。

---

## 与 AYShader 的分工

| 层 | 职责 |
|---|---|
| **AYShader** | Phoskia → `ShaderResource`；`setUniform` / `setTexture` / `submit(DrawCallContext)` |
| **AYRenderer** | `bgfx::init` / frame / view；VB / IB / transform |

最小 draw 路径见 `design.md` §4。

---

## 依赖

- 公开：AYShader、AYMath、AYIO、AYGameLoop、AYUI
- 内部：AYResource、AYFont、AYEventSystem
- bgfx、bimg（仅实现层；公开头不泄漏 bgfx 类型）

## 目录（当前）

```text
AYRenderer/
├── AYRenderer.h                 # 模块入口
├── include/AYRenderer/          # RenderScene、RenderTypes、UIRenderBackend 等
├── src/                         # Renderer、SubSystem 与 pass 实现
├── backend/                     # 后端适配
├── demo/
└── unittest/
```

## 变更记录（2026-07）

| 模块 | 文档 | 要点 |
|------|------|------|
| AYRenderer | `design.md` §10.4、§16 | Engine 集成、shutdown、ShaderPool 析构修复 |
| AYEntity | `design.md` §15 | `bootstrapModule`、`RenderSystem`、SparseSet 指针 |
| AYCore | `README.md` | `AYCore/CoreSerializer.h` 宏提升 |
| AYSerializer | `README.md` §变更记录 | 默认 `SerializerFor` → `SerializerForReflect` |

**里程碑**：R0–R4 + Engine 闭环已完成；R5+ 延后，可启动 AYUI。
