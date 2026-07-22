# F1 SIGSEGV — 定位手册（不要绕过）

**原则：** 绿测试 ≠ 根因已修。用开关隔离 + **Clean 全量重编** + `sizeof` 跨 TU 比对。

## 你已有的证据

| 证据 | 含义 |
|------|------|
| 栈在 `EventBusHostScope::subscribe` → `vector::_Orphan_all` 读 `0xFFFFFFFFFFFFFFFF` | **二次崩溃**：`_events` 所在对象布局错或堆已坏，不是 EventBus 业务逻辑必坏 |
| `RendererSubSystem` 含 `_scene`（`RenderScene`）再 `_events` | `RenderScene` 一变大，`_events` 偏移变；**混编新旧 obj** 会精确打出该栈 |
| 撤 Light / 重编后曾全绿 | 可能是 **ODR/增量编译** 被清掉，或 Light 相关布局嫌疑解除；**不能**当根因结案 |

## 诊断开关（CMake）

在配置 `AYRenderer` 时设置（**lib 与 Test 必须同值**，已 `PUBLIC` 传递）：

| 选项 | 默认 | 作用 |
|------|------|------|
| `AY_F1_DIAG_LIGHT` | OFF | `RenderScene::Light` + `_lights` |
| `AY_F1_DIAG_FRAME_SHADOW` | OFF | `FrameContext` shadow 槽 + `lastFrameShadowFbo` |
| `AY_F1_DIAG_DEFAULT_SHADOW` | OFF | 默认管线最前 `addPass(Shadow)` enabled |

示例（VS CMakeSettings / 命令行）：

```text
-DAY_F1_DIAG_LIGHT=ON
-DAY_F1_DIAG_FRAME_SHADOW=OFF
-DAY_F1_DIAG_DEFAULT_SHADOW=OFF
```

**每次改开关后必须 Clean + 重编 `AYRenderer` 与 `AYRenderer_Test`。**

## 必跑用例（`Test_F1_LayoutDiag.cpp`）

1. `f1_diag_sizeof_matches_between_test_tu_and_lib`  
   - **FAIL** → 混编/ODR，先 Clean，不要查 EventBus  
   - stderr 会打印 lib vs test 的 `sizeof`
2. `f1_diag_eventbridge_subscribe_after_noop_init`  
   - 炸在这里且 sizeof 一致 → 真堆坏 / 真逻辑问题  
3. `f1_diag_dual_renderer_then_eventbridge`  
   - 仅此项炸 → sticky Noop 多实例腐化嫌疑  

建议再跑：`RendererEventBridge` 全套 + 全量 `AYRenderer_Test`（改开关后连跑 2～3 次）。

## Bisect 矩阵（请回填）

每次一行：Clean 重编 → 跑 LayoutDiag + EventBridge + 全量。

| # | LIGHT | FRAME_SHADOW | DEFAULT_SHADOW | sizeof一致? | EventBridge | 全量 | 备注 |
|---|-------|--------------|----------------|-------------|-------------|------|------|
| 0 | OFF | OFF | OFF | | | | 安全基线（应绿） |
| 1 | ON | OFF | OFF | | | | 只动 RenderScene 布局 |
| 2 | OFF | ON | OFF | | | | 只动 FrameContext |
| 3 | OFF | OFF | ON | | | | 只默认挂 Shadow |
| 4 | ON | ON | OFF | | | | Light+Frame |
| 5 | ON | OFF | ON | | | | Light+Shadow |
| 6 | OFF | ON | ON | | | | Frame+Shadow（你上次「过了」附近） |
| 7 | ON | ON | ON | | | | 完整 C' |

## 如何判读

| 现象 | 结论 |
|------|------|
| 任一行 sizeof 不一致 | **增量编译/ODR**；加 CI 守门或改布局后强制 touch 相关 cpp |
| #0 绿，#1 在 Clean 后仍炸 EventBridge 且 sizeof 一致 | Light/布局有关的**真 bug**（少见，继续 ASan） |
| #0 绿，#1 Clean 后绿，仅增量改 Light 后炸 | **混编确认**；根因是构建卫生，不是业务代码 |
| #6/#7 炸在 shaderc / 第二 Renderer，sizeof 一致 | 回到 sticky Noop / shaderc 子进程线 |
| 全矩阵 Clean 后全绿 | 历史 139 多为混编或 flaky；仍保留 LayoutDiag 作回归 |

## 自动化跑过的结果（2026-07-21，`%TEMP%\build_ayrenderer.bat` / `f1_diag_*.bat`）

| 配置 | 结果 |
|------|------|
| 全 OFF（基线） | **465 PASS**（ctest） |
| 仅 `LIGHT=ON`（Clean 重配+全编） | **465 PASS** |
| `LIGHT+FRAME_SHADOW+DEFAULT_SHADOW=ON`（修掉 FrameContext 含 bgfx.h 之后） | **465 PASS** |

**结论：** 在 **Clean 全量重编且 lib/Test 宏一致** 时，C' 组合 **不能稳定复现** SIGSEGV。  
你之前的 EventBridge / `_Orphan_all` / `0xFFFFFFFFFFFFFFFF` 栈，与 **混编导致 `sizeof(RendererSubSystem)` 不一致** 高度吻合，而不是 EventBus 业务 bug。

**附带修掉的真问题：** `FrameContext.h` 曾 `#include <bgfx/bgfx.h>`，在 `AYRendererSubSystem.cpp` 里与 `MemorySystem::instance` **编译期撞名**。已改为只存 `uint16_t shadowFboIdx`，禁止 FrameContext 拉 bgfx。

---

## §5.5 退役（2026-07-22）

两个 F1 诊断编译开关（`AY_F1_DIAG_LIGHT` / `AY_F1_DIAG_FRAME_SHADOW`）**永久退役**：

- 它们曾是 §5.5 PR-F1' C' 禁止组合（Light struct + FrameContext shadow 写回 + 默认开 Shadow）的「打开会复现」按钮。
- E5 ship 默认开 Shadow **不带禁止组合**（Light struct 已删、FrameContext 写回已删、`lastFrameShadowFbo` cache 已删）─ 两个开关再没有东西可 toggle。
- 同步删除：CMake `option(AY_F1_DIAG_*)` × 2、`RendererSubSystem::diagFlagLight/FrameShadow` 静态函数、`Test_F1_LayoutDiag` 中的运行时 diag-flag 断言改为 `static_assert`。
- `include/AYF1DiagFlags.h` 保留为薄 include-only marker：两个宏硬编码为 0，让任何尚未编辑的旧 TU 中遗留的 `#if AY_F1_DIAG_*` 块自动选 diagnostic-off 分支。
- `Test_F1_LayoutDiag` 改名为「ABI / ODR / sticky-Noop 守门」─ sizeof 三项检查仍然是 EventBus `_Orphan_all` 历史 crash 的最佳防线。

本文件保留作历史教训；新发现问题请开新 issue，不要再启这两个 flag。
