# AYRenderer 测试维护

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

```powershell
pwsh -NoProfile -File scripts/tests/verify-module-test-inventory.ps1 -Module AYRenderer
```

## 当前验证与后续

Windows Debug 的 10 个完整分区全部通过，清单为 909 个用例、无遗漏或重复。
8 个 GPU 用例因未开启真实设备选项而跳过，不能声称 GPU 已验收。
本次快速层 0.35 秒，完整层 54.88 秒；UI 组 36.08 秒，是主要剩余耗时。
拆组优先降低定向编译和回归范围，尚未优化字体/文本夹具或解除后端资源锁。
后续应先测量 UI 字体夹具，再细分高成本路径和评估独立临时资源，不削减断言。
