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

公开头文件：`AYRenderer.h`、`AYRenderer/RenderScene.h`、`AYRenderer/RenderTypes.h`、`AYRenderer/RendererSubSystem.h`。**不含** `<bgfx/bgfx.h>`。

---

## 引擎集成（GameLoop + ECS）

最小链路：

```
EntitySubSystem::update  →  World::update  →  RenderSystem（注册 scene builder）
GameLoop::submitRenderCommands  →  RendererSubSystem::renderFrame  →  Renderer::render
```

1. **Win32 窗口** — 启动前调用 `RendererSubSystem::setBootstrapWindow(hwnd, w, h)`。
2. **子系统** — 链接 `AYEntity` + `AYRenderer` 后，`Entity` / `Renderer` 通过 `REGISTER_SUBSYSTEM` 自动注册；`RenderSystem` 把带 `Transform` + `MeshComponent` 的实体提交到 `RenderScene`。
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

ayt::entity::bootstrapModule();   // 静态库：显式注册 Entity + RenderSystem

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
- 启动时用 shaderc 编译内置 `vs_color` / `fs_color`（include: `bgfx-install/.../include/bgfx`）
- **Esc** 或关闭窗口退出

| 结果 | 含义 |
|---|---|
| BgfxSanity 正常、Demo 全黑 | 问题在 AYShader 编译/绑定或 AYRenderer 材质路径 |
| 两者都黑 | 优先查 bgfx 初始化、GPU 驱动、shaderc 编译日志 |

- 1280×720 Win32 窗口，蓝色旋转 cube
- 左上角 **debug overlay**（FPS、draw 数、backend、分辨率）
- **F9** 保存 `{assetRoot}/screenshot.tga` 与 `screenshot.png`（stderr 会打印完整路径）
- **Esc** 或关闭窗口退出
- 需要 shaderc 与 bgfx `common.sh`（CMake 自动探测 `thirdparty/bgfx`）

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

## 与 AYShader 的分工

| 层 | 职责 |
|---|---|
| **AYShader** | Phoskia → `ShaderResource`；`setUniform` / `setTexture` / `submit(DrawCallContext)` |
| **AYRenderer** | `bgfx::init` / frame / view；VB / IB / transform |

最小 draw 路径见 `design.md` §4。

---

## 依赖

- **AYShader** — `#include "AYShader.h"`（`ShaderResourcePool`、`ShaderResource`）
- **AYIO** — 文件读取（Phoskia / 贴图路径）；进程 API 不使用
- **AYResource** — R2b：`loadMesh` / `loadMaterial` / `loadTexture`（`.aymesh` / `.aymat` / `.aytex`）
- **AYDevice** — 窗口句柄与分辨率（bgfx 初始化）
- **bgfx** — 仅渲染器 TU 包含；材质/着色器绑定路径不持有 `bgfx::ProgramHandle`

---

## 目录（目标）

```
AYRenderer/
├── README.md
├── design.md
├── CMakeLists.txt          # R0
├── include/AYRenderer/     # R1 起
└── src/                    # R1 起
```

详细目录与 phase 切片见 `design.md` §12–§13、§16（变更记录）。

---

## 变更记录（2026-07）

| 模块 | 文档 | 要点 |
|------|------|------|
| AYRenderer | `design.md` §10.4、§16 | Engine 集成、shutdown、ShaderPool 析构修复 |
| AYEntity | `design.md` §15 | `bootstrapModule`、`RenderSystem`、SparseSet 指针 |
| AYCore | `README.md` | `AYCore/CoreSerializer.h` 宏提升 |
| AYSerializer | `README.md` §变更记录 | 默认 `SerializerFor` → `SerializerForReflect` |

**里程碑**：R0–R4 + Engine 闭环已完成；R5+ 延后，可启动 AYUI。
