# AYRenderer 测试维护

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
  需另行核对 history 拒绝与夹具契约；本轮不放宽断言或更改生产 TAA 掩盖失败。
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
