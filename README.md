# AYRenderer

## 相机视口与输出（2026-10-14）

RendererSubSystem::setCameraViewports 配置最多16个独立 Forward 视图，按 viewportId
先求相机再提取场景。Renderer::renderViews 提交完整验证后的窗口/离屏 batch；
createCameraOutput/cameraOutputTexture/destroyCameraOutput 管理可采样颜色和深度附件。
3D DrawItem.cameraLayerMask 独立于物理 mask，主视图和独立视图都支持镜头过滤。
独立视图不使用主视图的 Deferred、TAA/Motion/曝光历史或编辑器选中视图；WorldLit2D
在显式 Forward batch 中拒绝。详见 [相机系统](../../AYDocs/CAMERA-SYSTEM.md)。
CPU/Noop 用例 CameraViews；独立 D3D11 读回用例 CameraViewsGpu。
离屏 UI/场景预览跳过固定场景和相机 GPU view，单Pass离屏预算为145。

## Deferred Decal v1（2026-10-06）

GBuffer 后、Motion/SSAO/Lighting 前的投影贴花；`setDeferredDecals` 复制最多16项，
`setDeferredDecalsEnabled` 默认关闭。localToWorld 是正尺度、正交基仿射矩阵；
local [-1,1] 盒裁剪，+Z 面向接收表面，XY 为 UV。无纹理时也可投纯色贴花。
支持 optional sRGB albedo/alpha、linear tangent normal，以及 roughness/metallic 混合。
保留世界位置、AO/模型ID、emissive/coverage 和原深度；稳定列表顺序决定重叠结果。
两组全尺寸 MRT 交替读写、借用原深度；缺资源不发布部分结果；禁用时下一帧释放私有目标。
Editor 提供明确标注 test projector 的地面预览及 opacity，不是 ECS 创作/场景保存功能。
首版是正确性优先的全屏复制，每项一次draw；全尺寸两组颜色约40 bytes/pixel，
大量贴花尚无tile/culling/atlas优化。仅不透明/切孔接收，不改轮廓、深度或透明材质。

## 体积光／雾 v1（2026-10-06）

Deferred HDR：Lighting → SSR → DepthHaze → Volumetric → Transparent → Bloom。
`setVolumetricSettings` 默认关闭；Render 面板提供开关、密度和 Final/透射率/散射诊断。
8..64 次确定性中点积分，Beer–Lambert 消光 + HG 单次散射；一个按原场景索引选择的
方向光／点光／Spot，方向光 CSM/Spot 读取本帧阴影；点光全向阴影仍不支持。
全局或局部轴对齐盒介质，颜色为散射 albedo tint；天空参与积分，零密度无目标/绘制。
首版全分辨率，无 froxel、噪声介质、多灯累计、多重散射或独立历史；
透明自身尚未在介质中积分（在雾后合成），不冒充完整体积透明材质。

## 新画面能力：SSR v1（2026-10-06）

Deferred 在 Lighting 后、DepthHaze/Transparent 前提供不透明 SSR；默认关闭。
高级 Host 使用 `setSsrSettings`，Editor Render 面板提供开关、强度及
0 composite / 1 hit confidence / 2 reflected radiance 调试。需要活动 IBL v2，
屏幕外/遮挡/粗糙度超限回退原 IBL；命中替换 split-sum 镜面项，保留 diffuse/direct/emissive。
全分辨率有界 8..64 步 + 4 次二分；无独立时域历史、Hi-Z 或粗糙反射滤波，
不支持透明/折射/完整镜子。新增程序加入默认离线生产清单，不绕过 Phoskia。

## 默认离线 shader 生产流程（2026-10-05）

标准编辑器及 `ay_add_game_executable` 自动构建/部署 66 项内置 shader 清单，
覆盖当前 2D/3D、实例化、阴影、后处理、AA、选中覆盖层与 UI。构建入口为内部工具
`AYRenderer_ShaderBuild platform profile package-directory`；清单引用实际 Pass 源，非测试副本。
CMake 的 `AY_PRODUCTION_SHADER_PLATFORM/PROFILE` 可选择编译目标，Windows 默认 windows/s_5_0，
同时用于 D3D11/12。跨编译尚无 host-tool 集成，明确拒绝；其他后端需重新离线构建并设备验证。

Renderer 启动按可执行文件而非 cwd 查找 `Shaders`，只消费活动后端的 engine/project 包；
engine 覆盖不足、摘要错误、策略非法或目标不匹配均可报告。Release 默认不启用运行时 shaderc，
发布的 Debug 项目也由 `production.policy` 强制严格；开发 Debug 保留编译/热重载。
已有正常 acquire 会复用安装的字节，不需要各 Pass 维护预热句柄。Noop 测试保持原有兼容行为。
自定义源/宏集合不是 CMake 自动扫描游戏目录，而由 ProjectBuild 的选定内容闭包负责。
标准编辑器采用 authoring.policy（含Release），保留动态预览与DSL源编译；install同时携带
shaderc和include根，按exe相对位置发现，不依赖开发机路径。游戏ProjectBuild仍严格，
作者工具与发布游戏不混用角色策略。第三方 install 可调用内部
ay_install_production_shaders，将包安装在目标可执行文件旁。

AYRenderer 是 AY Engine 的**渲染器子系统**：基于 bgfx，负责帧调度、视口、几何提交、场景数据收集，并通过 **AYShader `ShaderResource`** 绑定 Phoskia 材质（不自行编译 shader、不在材质路径上暴露 bgfx program/uniform handle）。

完整设计见 [`design.md`](design.md)。

Phoskia Compute 第5项使用生产 pool 调度，在 D3D11/12 验证 compute→vertex 数据、
shared/global atomicAdd、buffer/group barrier、两种组数量及 compute→RGBA32F image 回读。
资源归 pool，native bridge 仅 Renderer 内部借用；详见 AYShader README 的线程/预算/同步限制。

Phoskia 模块引用设备验证位于 Test_PhoskiaGpu：仅改变被包含模块内容并保留时间戳，
相同根源码仍取得新程序，RGBA32F 回读值随模块函数变化。使用生产资源池文件入口，
不手写替代 .sc；语言/依赖边界见 AYShader README。

Phoskia 值结构体（2026-10-04）已回读构造、字段修改、helper 传参/返回和 bool 字段。
D3D11/12 Phoskia 组各 9 cases / 2242 assertions 通过，无 skip；不是 GPU buffer
struct 布局扩展，不支持直接结构体 UBO/storage 上传。

纯数学 helper 函数（2026-10-04）设备测试已覆盖 vertex/fragment 前向调用、提前
return、循环、bool/uint 和生成名字碰撞。D3D11/12 完整设备组各 18 cases /
8513 assertions 通过，无 skip；结构体和模块复用由后续阶段补齐。

Phoskia 受限循环设备证据（2026-10-04）：Test_PhoskiaGpu 覆盖可见索引赋值不影响
推进、嵌套 int 运算和 uniform/local 名字遮蔽。D3D11/12 各 7 cases / 2039 assertions
通过；使用独立进程与 RGBA32F 回读，未降低容差。语言限制以 AYShader README 为准。

测试入口、功能分组、夹具隔离与用例清单维护见 [`docs/testing.md`](docs/testing.md)。

### CPU 变形网格上传（2026-10-04）

高级 Host 可用 `Renderer::createMesh32(..., false)` 跳过特征线提取及线框 GPU 索引。
原五参数重载仍默认构建线框；solid 几何、local bounds 与 owner-thread 复制/销毁契约不变。
禁用线框的网格在全局 wireframe 模式下回退 solid。AYAvatar::Presenter 默认使用此选项，
需要线框检查时 `upload(frame, resolver, true)`。此优化仍重建 GPU mesh，不是动态 buffer 更新。
RenderResourceTests 验证两种上传的 solid buffers、bounds、线框索引与销毁。

### 场景规模化（2026-10-03）

共享 DrawList 已启用保守静态 3D 视锥剔除；非抖动相机与两像素边界保持 TAA 稳定。
屏幕外 Shadow caster 不受相机剔除影响。蒙皮/Morph/粒子/2D/选择描边暂不剔除。
自定义顶点位移需关闭 draw 的 `allowFrustumCulling`；全局关闭对照使用
`Renderer::setFrustumCullingEnabled(false)`。统计与限制见 [R6-5](docs/render-architecture-r6.md)。

场景 3D 相机通过 World-scoped provider 在几何提取前同步，Host/Edit 覆盖优先；
相机切换和显式 notifyCameraCut 统一重置 TAA/Motion/曝光历史。推荐游戏入口在
AYEntity 的 PerspectiveCameraComponent，而不是逐帧调用 Host override。

`setRigidInstancingEnabled` 默认开启：GBuffer 与 solid Shadow 合并连续同 mesh/material/
range/state 的非蒙皮不透明提交，保留源顺序。透明、cutout、Morph、2D、选择描边、线框
及自定义 Forward 保持逐对象；Motion 仍逐对象匹配 current/previous world，不共享历史。
`getFrameStats().instancedDrawCalls/instancedObjects` 按几何提交计数（阴影 cascade 会重复）。
48-byte 仿射数据兼容非均匀缩放/shear；不同镜像绕序拆批，退化变换或后端/瞬态容量不足
回退逐对象。新 shader 仍走 Phoskia/pool，无新增 raw-SC 分支。验收见 R6-5 与测试文档。

### 编辑器线程迁移状态（2026-10-03）

`RendererSubSystem` 的场景提取已使用 `RenderScene::DrawDataOwnership::OwnedCpu`：
每次 `add()` 立即复制骨骼矩阵/映射、2D 参数和 CPU 粒子顶点/索引，场景副本共享只读 backing。
直接构造的 `RenderScene` 默认仍是同步 Borrowed 模式，兼容原有独立宿主。
GPU 粒子 stream/callback 仍借用；mesh/material id 也不等于 GPU 资源租约。
`hasBorrowedGpuStreams()` 只检查已知 GPU 借用边界，不能证明整帧可跨线程。
**当前尚未启用编辑器独立提交线程**；UI 控件回调、GPU 资源命令、子窗口和 bgfx 生命周期
必须完成统一 owner-thread 接管后才能启用。迁移阶段与验收见 [`design.md`](design.md)。

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

测试按 fast/integration/gpu 分层，未启用真实设备时 GPU 用例明确跳过；见[统一测试契约](../../AYDocs/testing.md)。

## Editor diagnostic lines (2026-10-15)

Renderer::setEditorDebugLines owns a finite, bounded world-space ABGR line
batch. Invalid batches preserve prior state; empty clears. The default x-ray
route uses the unjittered main viewport camera in EditorOverlay after Present.
setEditorDebugDepthTest(true) routes the same owned geometry through
TransparentPass while scene color and depth are paired, using the scene's
jittered projection and LEQUAL, with no depth writes. Forward and deferred
pipelines use their existing scene/borrowed color-depth attachments.

Both routes share the cached line geometry/shader implementation, release GPU
resources through renderer teardown and preserve settings across pipeline
rebuilds. The built-in editor uses editor pipelines; generic game pipelines
omit the post-Present x-ray pass. Camera-specific sprite-overlay routing is
not part of this API. Tests: AYRenderer_EditorOverlay and the opt-in real D3D11
AYRenderer_PhysicsDebugGpu pixel fixture (colors, clear, forward/deferred occlusion).

Verification: EditorOverlay 15/15 and the isolated real D3D11 pixel case 1/1
passed (x-ray, forward/deferred occlusion and clear). Reports:
`out/debug-visualization-20261015/renderer-overlay.json` and `gpu-final.json`.

## Debug content extension (2026-10-15)

Opt-in setDebugCaptureEnabled/debugSnapshot copies the last main render: up to1024 draw classifications, mesh bounds, borrowed SceneLights values and produced shadow projection matrices. DrawItem appends optional Entity/World provenance. Visibility comes from the real frame-list classification; shadow casters remain independent. Deformed bounds are labeled bind-pose. This ordinary mesh entry exposes no LOD index; unrelated Avatar LOD policies are not inferred. Turning capture off clears observations; preview/multiview rendering does not overwrite the main snapshot.

Module-local regression sources were updated. Runtime tests and interactive editor acceptance are deferred by the current task request; previous validation reports cover the earlier baseline. These changes remain uncommitted until focused runtime checks are requested and pass.

## Extended diagnostic observations (2026-10-15)

UIRenderBackend::debugClip exposes the actual effective logical rectangular clip and stencil/path depth during an active frame through the optional UI backend observation seam. It returns invalid outside a frame; it does not infer path outlines. Screen diagnostics render through UI while world diagnostics retain the existing renderer debug line path.

Regression sources are included. Runtime tests and interactive editor acceptance are deferred for this content task; untested changes remain uncommitted.
