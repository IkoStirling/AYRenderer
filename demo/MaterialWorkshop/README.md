# Material Workshop

**Version:** 0.1.0

**Date:** 2026-10-20

**Status:** Active

**Owner:** AYRenderer / AYEditor


原生 PBR 材质创作样例，包含十七种表面、四种状态、参数对照、穿孔屏风与透明混合样片。
所有资源使用既有 `.mat`、`.msh`、`.scn` 和 `project.prj` 格式。
`Assets/materials/library.json` 是本演示的生成配方，不是新增引擎资产类型。

## 打开和编辑

在 Editor 的 Open Project 中选择本目录的 `project.prj`。工程包含九组场景与十七种材质近景，
各有基础、洁净、潮湿、磨损四种状态，共 104 个可编辑 Scene：

| 场景 | 内容 |
|---|---|
| gallery | 八种材质的球体、平面样片和石质展台 |
| facade | 石墙、瓷砖窗面、木饰板、金属管线、发光标牌 |
| workbench | 木工作台、橡胶垫、金属零件、油漆外壳和织物 |
| study | 木纹 UV 0.5/1/2/4、瓷砖法线 0/0.5/1/2、金属粗糙度偏置对照 |
| cutout | 基色 alpha、独立 opacity 红通道、独立 opacity alpha 的同形孔洞 |
| shapes | 八种材质分别应用于球、非均匀缩放球、圆柱、环面；按行对比形状和法线 |
| collection | 十六种不透明材质的总览，带编号与名称 |
| shapes-extra | 铜、混凝土、水磨石、皮革、沥青、砖、大理石、塑料的形状对照 |
| transparency | 亚克力薄板与透明盒体，观察背景和不透明物体穿过 Blend 表面 |
| hero-* | 十七种材质各自的球体、方体和长条近景，独立保存相机 |

原有钢、石、木、瓷砖、橡胶、油漆、织物、电路面板保留；新增铜绿、混凝土、水磨石、
皮革、沥青、错缝砖、脉纹大理石、塑料和透明亚克力。全部沿用现有单层 StandardLit。

在 Assets 中双击 `.mat` 打开独立材质窗口。可编辑颜色、PBR 因子、UV、法线强度、
粗糙度偏置、透明度来源、双面状态和七个贴图槽；球体、非均匀缩放球与 UV 平面提供实时预览。
拖动预览转动视角，滚轮缩放。Enter、失焦或 Apply 提交字段，Save/Ctrl+S 先提交有效字段再保存。
Undo/Redo 保留保存前后的历史，Reload 从磁盘重读；磁盘内容被其他程序修改时拒绝覆盖。

贴图字段填写项目源资源根的相对路径，例如 `materials/textures/wood_d.png`；
清空使用默认贴图。新增贴图必须已经存在且位于项目源资源根内。
独立 opacity 贴图还需选择红通道或 alpha 通道，透明度会与基色 alpha 和 Opacity 因子相乘。
File → New PBR Material 创建独立 `.mat` 与相邻 shader，不依赖此演示工程。
导入缓存或外部材质可只读预览，当前没有导入材质的 Save As/copy 流程。

预览采用独立工作室灯光、ACES 和 Gamma 2.2；未保存修改只作用于预览快照。
连续编辑、关闭预览再打开都保留 Scene 共享程序。保存写入材质文件，Scene 的更新走现有
资源热重载；禁用文件观察时需显式重载，未保存文档不会被 Scene 自动采用。
Editor Scene View 使用 Host 灯光。工程不创建未注册的 Light ECS 类型，灯光 rig 由 Native 展示器提供。
生成器为首次打开的 Scene View 写入初始视角，已有 `.ayeditor/workspace.json` 视角会被保留。

## Native 展示器

从引擎根目录构建 `AYRenderer_MaterialDemo`，运行后默认进入交互展厅：

```powershell
AYRenderer_MaterialDemo --scene gallery
AYRenderer_MaterialDemo --material ceramic --variant wet --lighting sunset
AYRenderer_MaterialDemo --scene cutout --animate --lighting-sweep
```

Tab 切换场景，M 切换材质近景，V 切换状态，L 切换灯光，A 切换表面参数动画，
S 切换灯光扫动，F 切换 Forward/Deferred，Space 暂停，F9 截图。
`--d3d12` 选择 D3D12，默认 D3D11；`--materials` 指定复制后的材质目录。
`--gallery --hidden --output <新目录>` 导出相同时间点的两路径；
`--motion-gallery` 每路径捕获 32 帧，按 30 次提交/秒推进、每三次提交采样一次。
参数对照保持各样片的固定值，表面动画作用于基础表面和屏风。

默认关闭阴影、TAA 和环境 cube，使用两路径共有的直射灯；
不把两路径不同的 IBL 实现宣称为相同光照模型。动态 UV 不包含前一帧材质 UV 速度。
全部表面均为现有单层 StandardLit 外观研究，潮湿/织物不是新的液体或 cloth BRDF。
亚克力使用普通 straight-alpha Blend，不含折射、透射 BRDF 或顺序无关透明；
透明对象仍受现有逐对象排序及单个 mesh 内部表面顺序的限制。

## 重建资源

先备份自己的编辑。Python 生成贴图与配方，C++ 工具使用真实 AYResource 编码器写 `.mat`：

```powershell
python AYRuntime/AYRenderer/demo/GenerateMaterialLibrary.py
AYRenderer_MaterialTool build AYRuntime/AYRenderer/demo/MaterialWorkshop/Assets/materials
AYEditor_MaterialSampleTool AYRuntime/AYRenderer/demo/MaterialWorkshop --overwrite
```

默认贴图为 256²，可用 `--size 512` 或 `--size 1024` 在指定 `--output` 目录重建更高分辨率。
法线由周期高度导数产生，不依赖 Pillow；分辨率变化保留相同 UV 空间斜率标度。
68 份基础/状态材质、48 份单参数研究、12 份剪裁研究共用 411 张 PNG，共 128 份 `.mat`。
新细胞纹理使用周期性的抖动单元，法线由相同周期高度场的包裹导数生成。
贴图中的 RGB 颜色按槽使用 sRGB，法线、金属、粗糙度、AO、opacity 使用线性数据。
材质持久化参数为 `uvTransform` Float4、`normalStrength` Float、`roughnessBias` Float；
UV 同时作用于全部表面贴图和 opacity，法线强度 0 使用几何法线，粗糙度最终夹到 0.045..1。

SampleTool 默认拒绝覆盖工程及已有生成网格/场景。`--overwrite` 只用于有意重建此生成样例；
多文件生成不是整库事务，失败时检查输出和错误记录。
样例使用可移植的 owner-relative 引用 `./textures/...`，保留 `textures/` 虚拟根的引擎含义。

## 导出与工具检查

```powershell
pwsh -File AYRuntime/AYRenderer/demo/ExportMaterialShowcase.ps1 -Executable <AYRenderer_MaterialDemo.exe> -OutputDirectory <新目录> -IncludeHeroes -IncludeAllConditions -IncludeMotion
python AYRuntime/AYRenderer/demo/ComposeMaterialShowcase.py <导出目录> <新浏览目录>
python AYRuntime/AYRenderer/demo/TestMaterialTools.py <AYRenderer_MaterialTool.exe>
python AYRuntime/AYRenderer/demo/TestMaterialDemo.py <AYRenderer_MaterialTool.exe> <AYRenderer_MaterialDemo.exe> <新验收目录> --d3d12
python AYRuntime/AYRenderer/demo/TestMaterialExport.py <AYRenderer_MaterialDemo.exe> <新验收目录>
```

导出器保留输入配方、shader、全部输入文件摘要、展示器程序摘要、实际完成章节与截图清单。
每章结束复核输入摘要，输入期间发生变化则停止且不接受该章；这不是文件系统快照事务。
失败不删除已完成输出。
整理器生成并排总览、材质近景、状态对照和三个动态 GIF，还生成可离线浏览的
`material-workshop.html`。页面内嵌实际截图的缩略图与 GIF，可按展厅、近景、状态、灯光、动态
选择章节；原始 PNG 链接依赖旁边的导出目录，移动交付时保留两个目录的相对位置。
它是内容工具；导出数量不代表测试覆盖。真实设备回归与边界见[模块测试说明](../../docs/testing.md)。
设备验收脚本需要 Pillow。它在复制的库中检查坏材质/贴图/作者 shader 被拒绝，
并用真实编码器构造 alpha=0 参考，与实际 Blend 样片逐像素比较；通用资源桥的备用 shader 策略保持不变。
