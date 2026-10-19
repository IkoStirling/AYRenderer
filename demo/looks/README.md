# Render look作品集

这些是完整的作者配置快照，可由Editor载入、修改、另存，或由演示Host显式加载。
它们不引用图片、纹理、GPU历史或Scene，尚未自动接入运行时ResourceManager/Cook/Pak。

## 选择内容

| 文件 | 内容 | 建议场景 |
|---|---|---|
| `0.rsl`…`9.rsl` | 原始十套起点；保留schema1迁移资料 | gallery、city、workshop |
| `arcade.rsl`、`broadcast.rsl` | 彩色显示器模拟；schema2 | character、city |
| `afterimage.rsl`、`stop-motion.rsl`、`tile-refresh.rsl`、`time-echo.rsl` | 四种时间内容；schema3 | character |
| `reference-palette.rsl` | 从同目录32色参考图提取的8色快照；schema3 | observatory、canal |
| `cel-ink.rsl`、`cel-paint.rsl` | 保色明暗层次；schema4 | observatory |
| `paper-echo.rsl` | 蓝色墨线、暖纸双色、轻网点和偏移残影；schema4 | character、canal |
| `phosphor-stop.rsl` | 七色绿磷光、3像素格、4提交停格、轻扫描线与曲率；schema4 | character、city |

最后两套是组合作者作品，不增加内置preset枚举，左右箭头仍循环18套起点。
`GenerateCombinedLooks.py`可从版本化基础配置重建它们；短JSON小数可直接读取，保存时输出规范Float32值。
纸墨颗粒seed固定，避免把随时间噪声误认为真实运动；Host可以显式推进seed。

## 导出与交互

从引擎根目录构建`AYRenderer_ColorStylizationDemo`和`AYRenderer_RenderStyleTool`。
例如，假设可执行文件已在PATH：

```powershell
AYRenderer_RenderStyleTool inspect AYRuntime/AYRenderer/demo/looks/paper-echo.rsl
AYRenderer_ColorStylizationDemo --motion-gallery --hidden --scene character --look AYRuntime/AYRenderer/demo/looks/paper-echo.rsl --output paper-echo
AYRenderer_ColorStylizationDemo --motion-gallery --hidden --scene character --look AYRuntime/AYRenderer/demo/looks/phosphor-stop.rsl --output phosphor-stop
AYRenderer_ColorStylizationDemo --look AYRuntime/AYRenderer/demo/looks/reference-palette.rsl --animate-palette --scene observatory
```

去掉批量参数可交互观看，F切换Forward/Deferred，Space冻结，Tab切换场景，F9截图。
`--d3d12`选择D3D12；导出固定步长序列，每条管线32张。组合作品需动态序列才能观察残影和停格。
色板动态配方循环原始→暖色→冷色，批量时固定几何以显示真实颜色变化。

Editor中将文件放到项目声明的assetRoot内，刷新资产浏览器后双击LOOK条目应用。
调整Render面板后另存作品；载入作品不修改Scene。切镜或需要重启残影时使用独立历史重置。

## 一条命令导出精选展示

```powershell
pwsh -NoProfile -File AYRuntime/AYRenderer/demo/ExportStyleShowcase.ps1 -Executable <built-demo.exe> -OutputDirectory <fresh-directory> -IncludeMotion
```

默认导出六个精选章节，每章两条管线各一张；`-IncludeMotion`追加五段序列，共332张PNG。
每个章节保留实际载入的`source-look.rsl`、日志和原生TGA；`showcase.json`记录后端、作品摘要与完成章节。
输出目录必须全新，已有内容会保留并拒绝覆盖。失败时停止后续章节，保留已完成章节和失败日志。
去掉`-IncludeMotion`仅12张静态PNG；`-D3D12`选择另一后端。源配置与捕获由同一原生demo消费，
该脚本不是新的运行时资产发布器，也不证明非当前平台的支持。
