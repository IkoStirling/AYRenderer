# AYRenderer 测试维护

## 2026-10-04 Phoskia 六项扩展：第 1 项类型与绑定

- D3D11/12 完整设备回归各 9 suites / 15 cases / 8166 assertions，通过且无 skip。
  新用例覆盖整数/布尔字上传（含 UINT32_MAX）、整数向量与数组、混合 std140 块、
  Mat3 padding 和跨块同名字段；既有 TAA、Motion、实例化与天空回读门禁保持通过。
- Renderer 10 个 CPU/契约测试组全部通过；普通 GPU 组的 skip 不作为设备证据。
  Shader 最终 fast 152 cases / 716 assertions，integration 310 / 1132，全通过。
  初次新增的拒绝用例使用了错误的 uniform block 测试语法，改为 uniformblock 后复测通过。
- 清单 Shader 462 / 2 partitions、Renderer 936 / 10 partitions，无遗漏或重复；
  SDK doctor 0 errors / 0 warnings，独立编辑器重建成功，未替换运行中的旧编辑器。
- 日志：out/phoskia-expansion-1-verified-cpu.log（Shader 拒绝用例修订前）、
  out/phoskia-expansion-1-boundary-final-test.log（Shader 最终复测）、
  out/phoskia-expansion-1-verified-d3d11.log、out/phoskia-expansion-1-verified-d3d12.log、
  out/phoskia-expansion-1-verified-sdk.log。

## 2026-10-04 Phoskia 加固第九阶段与交付状态

- 删除源码红测试 4 assertions 失败，修复后删除/重建/再删除及独立 handle 对照通过。
  Shader fast 150/695 assertions、integration 308/1118 assertions，26.60 秒全部通过。
- 第九阶段重建后 Renderer 10 组全部通过（76.37 秒）；D3D11/12 Phoskia 各
  4 cases/1352 assertions 无 skip。完整设备组在第八阶段各 14/7691 已通过，第九阶段
  只改变文件删除监听失效通路，没有改渲染算法、二进制生成或缓存 schema。
- SDK doctor 0 errors/0 warnings；Shader 清单更新为 458 cases/2 partitions，无遗漏/重复。
  独立编辑器最新重建成功：out/build/render-phoskia-20261004/AYRuntime/AYEditor/AYEditorShell_Demo.exe。
  运行中的旧编辑器/并行任务未替换；此处不宣称完成用户视觉验收。
- 本轮按至少 3 小时要求实施，开始为北京时间 2026-10-04 14:02:27；主线外增加
  compute、配置快照、shutdown、向量分量、声明检查、缓存边界与文件删除等独立修复。
  AYShader design.md 保留已解决问题、证据及后续边界，不能视为语言/后端全部完备。
- 日志：out/render-phoskia-stage9-red.log、out/render-phoskia-stage9-build.log、
  out/render-phoskia-stage9-shader.log、out/render-phoskia-stage9-renderer.log、
  out/render-phoskia-stage9-d3d11.log、out/render-phoskia-stage9-d3d12.log、
  out/render-phoskia-stage9-sdk.log 与 out/render-phoskia-stage9-inventory.log。

## 2026-10-04 Phoskia 加固第八阶段与全量收尾

- raw stage 边界碰撞红测试 3 assertions 失败；长度编码后拒绝非法 B，不命中有效 A。
  缓存 v11，磁盘持久化/旧 v10 隔离、配置快照及 shutdown 回归保持通过。
- 最终 CPU 12 个 CTest 入口全部通过（96.32 秒）：Shader fast 150/695 assertions、
  integration 307/1103 assertions；Renderer 10 组，普通 GPU 组 14 skips 明确不作设备证据。
- D3D11/12 设备回读结果见 out/render-phoskia-final-d3d11.log 与
  out/render-phoskia-final-d3d12.log；每组 9 suites/14 cases/7691 assertions，原验收容差不变。
- Renderer 清单 935 cases/10 partitions、Shader 457/2 partitions 无遗漏/重复。
  SDK doctor 0 errors/0 warnings；独立编辑器完整重建成功，运行中的旧进程未替换。
- 日志：out/render-phoskia-stage8-red.log、out/render-phoskia-final-cpu.log、
  out/render-phoskia-final-rebuild.log、out/render-phoskia-final-sdk.log、
  out/render-phoskia-final-inventory.log 与 out/render-phoskia-final-shader-inventory.log。

## 2026-10-04 Phoskia 加固第七阶段

- 声明语义红测试 25 个断言失败；UBO 类型/声明顺序、字段与重复检查、property
  初始化器诊断修复后 Shader 两个 CTest 入口全部通过（25.16 秒）。随后新增的
  program/IR 拒绝入口回归单独通过 1 case/18 assertions，无 skip。
- 独立 AYEditorShell_Demo 与全部 Renderer test groups 重建成功。未替换运行中的旧
  编辑器，重建不等同于视觉验收。cache schema v10；后续最终 GPU 验证另记录。
- 日志：out/render-phoskia-stage7-red.log、out/render-phoskia-stage7-shader.log、
  out/render-phoskia-stage7-entry.log、out/render-phoskia-stage7-build.log 与
  out/render-phoskia-stage7-sdk.log。

## 2026-10-04 Phoskia 加固第六阶段

- Vector conversion 曾复制首分量：真实 D3D11 红测试 128 个数值断言失败，修复后保留
  分量，并对 D3D 显式截取后转换；scalar splat、Mat3 上传与缺分量拒绝都有对照。
- Shader fast 150/695 assertions、integration 301/1044 assertions 实际通过（23.49 秒）。
  Renderer 10 个完整组通过（75.23 秒）；其中普通 gpu 组 14 个明确 skip，不作设备证据。
- 独立 D3D11/12 各 9 suites/14 cases/7691 assertions 实际通过，无 skip；单独 Phoskia
  GPU 为 4 cases/1352 assertions。原 1e-4 数值容差和 TAA/Shadow 测试验收线未降低。
- SDK doctor 72 capabilities/53 modules/158 symbols，0 errors/0 warnings；cache schema v9。
- 日志：out/render-phoskia-stage6-red-gpu.log、out/render-phoskia-stage6-red-text.log、
  out/render-phoskia-stage6-shader-final.log、out/render-phoskia-stage6-renderer-full.log、
  out/render-phoskia-stage6-full-d3d11.log、out/render-phoskia-stage6-full-d3d12.log 与
  out/render-phoskia-stage6-sdk.log。初次仅恢复 GLSL ctor 的尝试在 D3D 出现 X3014，
  最终显式 swizzle 后全量通过。

## 2026-10-04 Phoskia 加固第五阶段

- 关闭期间的旧请求不得回填资源池。红测试先复现 6 个失败断言；内部 lifecycle epoch
  修复后，旧请求报告 cancelled，新请求重新生成 IR，取消请求不写磁盘成功条目。
- Shader fast 150/695 assertions、integration 300/1042 assertions，Renderer Shader
  11/84 assertions 均通过，联合 26.74 秒。D3D11/12 Phoskia 各 3/946 assertions 通过。
- SDK doctor：72 capabilities/53 modules/158 symbols，0 errors/0 warnings；公开布局不变。
- 日志：out/render-phoskia-stage5-red-tests.log、out/render-phoskia-stage5-fixed-tests.log、
  out/render-phoskia-stage5-final-tests.log（含磁盘检查）、out/render-phoskia-stage5-d3d11.log、
  out/render-phoskia-stage5-d3d12.log 与 out/render-phoskia-stage5-sdk.log。

## 2026-10-04 Phoskia 加固第四阶段

- 资源池编译配置、key 与磁盘目录使用同一请求快照；错误热重载版本不会成功缓存，
  恢复版本得到新绑定。Shader fast 150/695 assertions、integration 299/1026 assertions
  与 Renderer Shader 11/84 assertions 全通过，联合 27.58 秒。
- D3D11 与 D3D12 各 9 suites/13 cases/7285 assertions 实际通过，无 skip；覆盖 Phoskia、
  TAA（support/resident/rejection/edge）、motion vector、实例化及天空输出。
- 独立 AYEditorShell_Demo 最新重建成功，未替换 pid 69276 的运行版本，未宣称视觉验收。
  SDK doctor：72 capabilities/53 modules/158 symbols，0 errors/0 warnings。
- 日志：out/render-phoskia-stage4-red-tests.log（配置竞态先复现）、
  out/render-phoskia-stage4-fixed-tests.log、out/render-phoskia-stage4-final-tests.log、
  out/render-phoskia-stage4-d3d11.log、out/render-phoskia-stage4-d3d12.log、
  out/render-phoskia-stage4-editor-build.log 与 out/render-phoskia-stage4-sdk.log。

## 2026-10-04 Phoskia 加固第三阶段

- Compute 前端/IR/后端契约修复，缓存 schema v8。Shader fast 150/695 assertions、
  integration 297/1003 assertions 普通模式全部通过（25.12 秒），无 skip。
- Renderer Shader 11/84 assertions 通过；现有 Phoskia 真实回读在 D3D11/12 各
  3 cases/946 assertions 通过。这是材质路径的回归证据，不等于 compute 的 GPU 调度验收。
- 日志：out/render-phoskia-stage3-red-tests.log（22 个先复现失败断言）、
  out/render-phoskia-stage3-shader-final.log、out/render-phoskia-stage3-cpu-tests.log
  （Shader 首轮 uvec 构造器遗漏已修复，Renderer Shader 组通过）、
  out/render-phoskia-stage3-d3d11.log 与 out/render-phoskia-stage3-d3d12.log。
- SDK doctor 第三阶段实际通过：72 capabilities / 53 modules / 158 symbols，0 errors / 0
  warnings。此前并行音频接口 Doxygen 错误已由该任务修正，本任务未改其接口；根 SDK
  文件仅局部暂存渲染对应 hunk，保护同文件音频改动。

## 2026-10-04 Phoskia 加固第二阶段

- 新增独立 `AYRenderer_PhoskiaGpu`，归属 Gpu 测试组，使用隐藏窗口和既有
  `AY_TAA_GPU_TEST=d3d11/d3d12` 开关。未启用设备测试时显式 skip，不计为 GPU 验收。
- GPU 定位并修复标量 uniform 的物理槽与逻辑类型错配，以及 arbitrary sampler 名称的
  错误 stage 覆盖。回读检查运算结合性、条件/else-if、4/16-byte scalar、Mat4、Vec4、
  短 Vec3、scalar array、单元素数组、property 上传、局部/分支遮蔽和两种 sampler 顺序。
- Shader fast 150/695 assertions、integration 290/950 assertions；D3D11/12 各
  9 suites/13 cases/7285 assertions，全部实际通过。未修改原 TAA/Shadow/Motion 算法或容差。
- 六个 CPU 目标实际通过：Contracts 125、SceneMaterial 71、Geometry 96、
  LightingShadow 137、PostProcess 252、Shader 11 cases。golden 源文件结尾空行同步
  后重新构建复制并在非 regen 模式重跑 Shader 两层，均通过（23.06 秒）。
- 日志：`out/render-phoskia-stage2-uniform-tests.log`、
  `out/render-phoskia-stage2-all-d3d11.log`、`out/render-phoskia-stage2-all-d3d12.log`。
- CPU/Shader 最终日志：`out/render-phoskia-stage2-cpu-final.log`（渲染六组）、
  `out/render-phoskia-stage2-shader-final.log`（golden 修正后的 Shader 两层）。
  新 shadowing 用例先捕获 D3DCompile X4000（initializer 读未初始化同名局部），生成
  独立局部名后数值回读通过。golden 更新仅为 Vec4 物理声明和逻辑分量读，不替代 GPU 证据。
- 最新完整编辑器重建暂被并行 AYAudio 的 `pullOneFrame` 未定义阻断；该模块有并行 dirty，
  本任务未修改它，也未把先前成功构建描述成已含最新 Shader 修改的编辑器验证版本。

## 2026-10-04 Phoskia 加固第一阶段

- 独立 VS 2026 Insider Debug 构建：`out/build/render-phoskia-20261004`。
  AYShader 关闭 BGFX 未实现 stage-body 语句的静默成功路径，缓存语义 v6。
- Shader fast 150/150、integration 284/284；D3D11 与 D3D12 各 8 suites/10 cases/
  6339 assertions 通过，无 skip。Renderer 生产算法和既有 GPU 容差未修改。
- 根日志：`out/render-phoskia-stage1-final-tests.log`、
  `out/render-phoskia-stage1-d3d11.log`、`out/render-phoskia-stage1-d3d12.log`。
  初次未知 IR 测试夹具缺少 vertex，修正夹具后全量重跑通过；首次 CTest 的 CMD 正则
  转义未选中测试，不计作通过，最终使用 PowerShell 正则实际执行。
- 这些结果验证编译链和上述生产 Pass 的 GPU 回归，不代表完整编辑器视觉验收。

## 2026-10-03 TAA fixed-grid 断言修复

- 根因在 AYShader，不是 ABI、实例合批或 TAA 权重预期：Phoskia `else if` 被错误恢复，
  历史混合/debug/return 被吸入 sky 分支；Lexer 也截断双字符运算符，编译入口忽略了
  非空 recovery AST 的 parser errors。修复通用编译链路，pool cache schema 更新为 v5。
- 原用例的 8 个 history feedback 和 1 个混合边缘失败全部恢复通过，容差未放宽；
  增加实体路径五种 debug mode 与 missing-motion rejection 的真实 GPU 回归。
- VS 2026 Insider Debug：D3D11、D3D12 各 8 suites/10 cases/6339 assertions 全部通过，
  覆盖 fixed-grid、support confidence、resident ping-pong、real GBuffer rejection、
  sloped/recursive edge、Motion、Instancing、Sky。原静态边缘少量覆盖率波动仍在，
  不把门禁通过描述成所有画面完全无波动或完整编辑器视觉验收。
- CPU 定向结果：Shader fast 150、integration 280；Renderer Contracts 125、SceneMaterial
  71、Geometry 96、LightingShadow 137、PostProcess 252、Shader 11，全部实际通过。
  最终联合 CTest 曾有 SceneMaterial 启动 `operation not permitted`（未运行），单独重跑
  71/71 通过；不能把首次未启动写成用例通过。构建 PDB 占用在单线程重试后完成。
- 根输出日志：`out/taa-fix-d3d11.log`、`out/taa-fix-d3d12.log`；CPU 详细记录
  `out/taa-fix-final-cpu.log` + `out/taa-fix-final-scene.log`，编译器/Shader 独立门禁
  `out/taa-fix-final-shader.log`。SDK doctor：68 capabilities/52 modules，0 errors/0 warnings。
  `AYEditorShell_Demo` 已重建；未自动打开或完成用户场景交互验收。

## 2026-10-03 场景规模化三阶段

- 静态 culling 的六面/near/未知 bounds/变形回退/稳定 jitter 与 Motion 重入回归归属 Contracts。
- Scene Camera：Entity focused 6/6、Entity 全量与 Renderer provider/Host 覆盖/撤销回归通过。
- Instancing：Contracts 125、Geometry 96、SceneMaterial 71、PostProcess 252 通过；Shader
  前端/IR/实例属性与 s_5_0 编译通过。实际命令/输出保存在根 `out/render-scale-stage*-*.log`。
- `AYRenderer_InstancingGpu` 属于 `Test_TAAGpu.cpp` / Gpu 组，必须在独立进程运行：
  `$env:AY_TAA_GPU_TEST='d3d11'`（或 `d3d12`），再运行 `AYRenderer_GpuTests.exe --suite AYRenderer_InstancingGpu`。
  隐藏窗口、无编辑器输入；两后端各 1 case/20 assertions 实际通过，6→2 draws，法线和阴影
  readback 一致。未设置环境变量会显式 skip，不得把 Noop/skip 计为 real-GPU 验收。
- `AYEditorShell_Demo` 已重建；新功能的完整 Edit/Play 交互和大场景性能仍需用户实际验收。
- 最终定向层：Shader 278、Contracts 125、SceneMaterial 71、Geometry 96、LightingShadow
  137、PostProcess 252 全部通过；Entity 全量、camera 6/6、Editor 353/353 通过。
- 补跑真实 GPU TAA 暴露 `taa_production_shader_fixed_grid_depth_and_missing_velocity` 的
  9 个失败断言（8 个 history feedback、1 个混合边缘）；单独进程仍复现。用 AYShader
  提交前的 Lexer/Parser/Semantic/IR/BGFX 源码在 `out/render-scale-baseline` 隔离重编/重链接，
  未修改的生产 TAAPass 仍输出同样数值：中心约 0.4578，夹具期望约 0.45965。
  本轮实例化不经过该 TAA 夹具，旧编译器也复现，不能将此项宣称为通过或归因于合批。
  此失败随后已定位并修复，见上方 TAA fixed-grid 专项记录；三阶段原始证据保留。
  MotionVectorGpu（静态/移动/蒙皮/两投影/8 jitter phases）通过。
  最终 Instancing+Motion 联合门禁在 D3D11/D3D12 各为 2/2 cases、792/792 assertions。

## 2026-10-03 编辑器线程迁移阶段 1 定向验证

- `Test_RenderSceneOwnership.cpp` 归属 Contracts，新增 8 个 CPU 所有权/生命周期用例，
  包括 producer scope 退出、完整骨架、场景复制/移动/clear/扩容和另一线程持有 CPU 副本。
- VS 2026 Insider Debug 实际通过 Contracts 117/117、Geometry 96/96、SceneMaterial 69/69，
  编辑器全量重跑 353/353，`AYEditorShell_Demo` 构建通过。
- 编辑器全量首次有一个恢复会话用例失败，定向与全量重跑通过；该间歇失败未在本次修改中修复。
- 实际窗口烟测覆盖启动、视口 picking/Inspector、Play/Pause/Stop、放大/恢复和正常退出；
  CPU 所有权验证不等于完整引擎独立 render owner、GPU stream lease 或 UI 并行验证。
- `RenderScene` 布局变更必须重编消费者；本次先统一 MSVC 环境并重建依赖，确认 Ninja
  已重新记录头文件依赖，不能用旧零依赖对象文件作为 ABI 回归证据。

下表是 2026-09-28 的历史完整层统计，阶段 1 的新增用例与实际定向数量见上。

## 分组与统计

2026-09-28 Windows Debug 实际注册 909 个用例，完整层按功能拆成 10 个独立程序。
每个测试源文件独立编译，由 OBJECT target 直接链接注册器；不使用文本包含、Unity
编译或依赖静态库链接器保留注册器。`unittest/TestGroups.cmake` 是源文件归属清单。

| Group | 用例数 | 层级 | 范围 |
| --- | ---: | --- | --- |
| Contracts | 108 | fast | 矩阵、可见性、资源/Pass/布局与纯数据契约 |
| SceneMaterial | 69 | integration | 场景、材质、选择与编辑器覆盖层 |
| Geometry | 96 | integration | 几何、2D 提交、蒙皮与网格 |
| LightingShadow | 137 | integration | 灯光、阴影与反射 |
| PostProcess | 252 | integration | 后处理、TAA 与图计划 |
| FrameGraph | 41 | integration | 帧图、管线与资源生命周期 |
| UI | 114 | integration | UI 渲染、字体和文本 |
| Shader | 11 | integration | 着色器、资源桥与阴影配置 |
| Audit | 73 | integration | Pass 实现审计与回归 |
| Gpu | 8 | gpu | 真实设备读回验收 |

fast 从 34 个扩充为 108 个经独立运行验证的自包含用例；integration 为 793 个，
gpu 为 8 个。注册清单而非断言数是统计依据。CTest 的 10 个条目是分区，不是用例。
层级按夹具依赖分类；功能组名称本身不意味着所有用例都依赖真实后端。

## 日常运行

先配置 `windows-debug`，在已加载 MSVC 构建环境且 CMake/CTest 可用的终端运行：

```powershell
pwsh -NoProfile -File scripts/tests/run-renderer-tests.ps1 -Tier fast
& ./scripts/tests/run-renderer-tests.ps1 -Tier integration -Group Shader,FrameGraph
pwsh -NoProfile -File scripts/tests/run-renderer-tests.ps1 -Tier full -Audit
pwsh -NoProfile -File scripts/tests/run-module-tests.ps1 -Module AYRenderer -Tier fast
```

`-Group` 只构建和运行所选功能组；多个值使用 PowerShell 数组调用，如上面的 `&`。
未指定 Group 时按层级选择全部对应组。组与层级不匹配会报错，不默默执行空集合。
`-List` 仅列注册项，`-SkipBuild` 复用已有程序，`-Audit` 校验完整层实际清单；
审计需要全部组程序，非 List/SkipBuild 模式会主动构建它们。

单用例筛选和问题报告仍由 AYTest 提供：

```powershell
out/build/windows-debug/AYRuntime/AYRenderer/unittest/AYRenderer_ShaderTests.exe --list
out/build/windows-debug/AYRuntime/AYRenderer/unittest/AYRenderer_ShaderTests.exe --suite AYShadowConfig --case simple_lit_shadow_phoskia_frontend_compiles --report-json shader-report.json
```

构建 target：`AYRenderer_TestGroups` 构建全部 10 组，
`AYRenderer_IntegrationTests` 构建 8 个 integration 组。旧 `AYRenderer_Test`
保留为手动兼容程序，需显式构建，不再注册 CTest，以免完整层重复执行用例。
客户端 2D/3D 构建预设已改为构建分组 target。旧 2D/MixedSceneVisibility 专项门禁
保留原名并指向相应分组程序，但不带 `renderer-full` 标签。

## 夹具与维护约束

- 新增 `.cpp` 必须在 TestGroups 清单中出现恰好一次；CMake 配置阶段检查缺失、
  重复与不存在的源文件。新增用例后运行公共清单审计，检查完整层遗漏和重复身份。
- Contracts 超时 30 秒，integration 组超时 180 秒，Gpu 超时 240 秒。
  integration 组及涉及后端的专项门禁共用 `renderer-backend-fixtures` 资源锁；
  外部暂存路径尚未全面审计，不承诺这些组可安全同时运行。Gpu 使用 RUN_SERIAL。
- 每个依赖 GPU 对象的夹具必须自行取得后端生命周期。绑定测试渲染器类型并不等于
  初始化后端；Shader 的 Phoskia 前端回归已补上自己的 Noop Renderer 生命周期，
  避免依赖其他套件先运行。
- GPU 默认明确跳过。显式设置 `AY_TAA_GPU_TEST=d3d11` 或 `d3d12` 后运行 gpu 层，
  才能验收真实设备；Noop 回归不能替代真实 GPU 覆盖。
- 后处理 View 顺序回归按依赖先后检查，不绑定数组绝对下标；GPU 粒子 compute
  必须早于 3D Transparent 与 2D overlay，且不能改变 HDR、后处理和覆盖层的顺序。

```powershell
pwsh -NoProfile -File scripts/tests/verify-module-test-inventory.ps1 -Module AYRenderer
```

## 当前验证与后续

Windows Debug 的 10 个完整分区全部通过，清单为 909 个用例、无遗漏或重复。
8 个 GPU 用例因未开启真实设备选项而跳过，不能声称 GPU 已验收。
本次快速层 0.35 秒，完整层 54.88 秒；UI 组 36.08 秒，是主要剩余耗时。
拆组优先降低定向编译和回归范围，尚未优化字体/文本夹具或解除后端资源锁。
后续应先测量 UI 字体夹具，再细分高成本路径和评估独立临时资源，不削减断言。
