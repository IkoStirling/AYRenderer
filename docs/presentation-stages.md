# 可选颜色阶段的扩展与执行

**Version:** 1.1.0
**Date:** 2026-10-29
**Status:** Active
**Owner:** AYRenderer

## 1. 范围

主视口 AA 之后、Present 之前的可选颜色链使用内部 `PresentationPass`。
目前覆盖 Outline、ColorGrading、ColorStylization、TemporalFeedback、AnalogDisplay。
公开 Renderer 设置、作品文件和 Editor 作者入口仍由各自契约维护；该接口不是游戏侧插件 API。
HDR、几何、光照、透明、AA、独立相机和 UI 保留原有时序与资源职责。

逻辑效果、C++ Pass、GPU 提交次数分别表达。一个阶段可以拥有多个内部 GPU 步骤。
共同约定是消费当前有效颜色，在成功提交后发布新颜色，不要求复杂算法压成一次 draw。

## 2. 声明与实现

[`PresentationStages.def`](../src/detail/PresentationStages.def) 声明槽、输出、View、顺序、输入族、
采样方式、历史输出、离线 shader job 和末尾直接显示能力。工厂、默认产品链、静态合约、颜色链排序、View/计时归属和
shader inventory 使用同一份声明。已有槽、资源和 View 数值保持稳定。

具体 `.cpp` 用 `AY_DEFINE_PRESENTATION_STAGE` 提供具体类型、实际 shader 源码和额外静态读依赖。
普通颜色阶段提供空依赖；Outline 和 TemporalFeedback 在自己的实现中声明几何和历史输入。
公共工厂和 shader catalog 不包含具体效果头，也不增加效果专用分支。遗漏实现会产生链接错误，
不依赖全局可变注册表或静态注册器初始化顺序。

全部固定主 View 的顺序与计时归属在 [`RenderViews.def`](../src/detail/RenderViews.def) 维护，
颜色阶段从阶段声明自动展开。UI255 保留默认最后 rank，不提前到动态相机/UI View 之前。
编译期检查重复 View、阶段顺序及输出合同；测试检查保留区和实际 shader job 覆盖。

## 3. 新增普通颜色阶段

1. 为新槽追加 `RenderPassSlot`；在 `RenderResourceIds.def` 追加共享颜色输出一行，保持旧值。
   两种资源枚举、Count、诊断名及对应关系自动生成，不分别修改两份枚举和名称数组。
2. 实现 `PresentationPass` 子类的 `active()`、`execute()`、`destroyResources()`。
   `resolveTargets()` 取得当前输入与输出目标/原点，成功提交后 `publishOutput()`。
   使用返回的 `x/y` 设置 View，不自行假定输出总从纹理原点开始。
   默认关闭时不取得 program、geometry 或目标。
3. 在本阶段 `.cpp` 定义 `AY_DEFINE_PRESENTATION_STAGE`。源码和额外依赖必须有稳定寿命，
   不返回临时字符串或容器。
4. 在阶段声明增加一行，按颜色空间和既定效果顺序选择位置；View 撞区由检查拒绝。
5. 在算法归属中接入参数、作者界面及必要的格式迁移，加入独立参考和失败/关闭验证。

已有下游阶段不追加新输出的候选输入项。普通阶段不修改主 Renderer 的资格分支、生命周期遍历、
默认链、计时 switch 或公共 shader job 清单。资源诊断名从输出声明取得。
身份追加、算法、参数/作品格式和验收仍是新增能力自身的工作。

## 4. 附加资源与历史

`preparePresentationResources()` 贡献当帧额外 reads/imports。消费者仍须验证输入是否当前、
尺寸/代际是否匹配，不能把有效 handle 当作已生产内容。静态依赖只声明真正额外需要的资源，
不列举所有可能的前驱颜色效果。

时间阶段自行准备、持有和释放历史，通过 `historyTarget` 提供本帧写入目标。
`finishPresentationFrame()` 收尾未提交帧，`invalidatePresentationHistory()` 处理切镜、
管线替换和上游不可用。需立即回收的历史在禁用配置回调中释放。
TemporalFeedback 保留两张 RGBA8、成功提交计数、viewport 原点/尺寸及 camera cut 规则。

这些 hook 不自动实现新的 Motion Vector、Hi-Z 或跨视图生产者。读取新上游能力时仍需定义
并验证相应生产契约，不能仅凭声明读取就假定前置算法已经存在。

`RenderResourceIds.def` 只集中身份及对应关系，格式、尺寸、寿命和所有权由各写者契约决定。
SSAOTexture/SSAO 显式对应；BaseGBuffer/DecalGBuffer 等不同边界各自保留，不能按数值强转。
两套已有编号及 HazeHalf 别名保持不变。编译期拒绝重复编号、空洞和超出8位空间的声明。

## 5. 每帧门控和失败

Renderer 准备已挂载阶段的贡献，纯 [`PresentationStagePlan`](../src/detail/PresentationStagePlan.h)
按顺序连接当前颜色并声明图节点。静态描述验证全部挂载前驱的顺序，关闭效果不构成合法重排。
`PostProcessGraphPlanInput` 只接收准备好的颜色阶段贡献，空链也有明确含义。
旧算法测试的命名开关在 `unittest/PostProcessGraphPlanFixture.h` 转换，生产代码没有逐效果兼容分支。

关闭或被裁剪的阶段不提交 GPU 工作。执行失败不提升 `PresentSource`，后续读取最近成功发布的上游。
准备失败的历史不能继续有效。发布表示提交，不表示设备 fence 或 GPU 已完成。
重复槽、重复输出、逆序、不兼容输出和覆盖颜色生产者的额外 import 有独立诊断。
共享 import 必须指向同一目标，且不能覆盖已有上游声明；报告携带阶段、资源名及原因。
固定 View 还会检查相机64..111、回读112和默认UI255保留区。注册的新主 View 会自动参与
离屏UI的避让，资源/视图预算变化仍需验收。

准备 hook 只能贡献本阶段资源和资格，结构身份由基类固定。准备返回不可用或抛出异常时清掉
部分贡献；异常同时尝试失效本阶段历史，并允许后续阶段继续准备。正常资源准备失败时，
历史 owner 仍负责记录具体失效原因。阶段停用后的旧 external handle 不计入本帧逻辑资源数。

装配时建立阶段索引，每帧不重复 RTTI 扫描全部 Pass。Renderer 重用贡献容器容量，逐帧清除旧的
import、history target 和读依赖；重用 CPU 容器不会缓存历史有效性或 GPU handle。
目标池、program cache 和私有资源继续由原 owner 管理。

## 6. 验证及融合

`Test_PresentationStages.cpp` 覆盖32种效果组合、四种AA、额外输入缺失、非法声明、准备/提交失败、
存储重用和完整 View 归属，并插入既有清单外的测试阶段，证明下游无需识别其名称。
测试阶段在隔离图中使用未占用的资源身份，不新增生产效果或开放运行时注册。
`Test_PhoskiaGpu.cpp` 验证真实效果、两条主管线、viewport、历史失效和回收。

融合是独立执行策略：先证明顺序、采样位置、格式量化、历史及中间消费者语义，再选有限预编译组合
并测量实际 GPU 工作。该机制不自动拼接任意 shader，也不建立第二套 RHI。
同组轻量操作可以继续扩展既有 shader，不必新增独立 Pass。

第一项整合是末尾 AnalogDisplay 直接显示。Advanced Host 的
`RenderPipelineDesc::integratePresentation` 默认开启；仅在受验证的 D3D11/12、0/2x/4x MSAA
默认 backbuffer 路径上，且该效果是最后启用的颜色阶段时生效。其他条件保留独立 Present。
该策略保留逻辑颜色链，省去 DisplayColor 的全屏 RGBA8 目标和 Present 的一次采样复制；
不把多个不同采样算法任意拼成 shader。后续若增加颜色消费者，直接显示自动停止，
新消费者不需要识别 AnalogDisplay。编译完整图时，任何启用的非Present纹理消费者或声明语义
引用该输出，也会恢复普通目标；按执行槽识别Present边界，不依赖名称。关闭的额外消费者不阻止
整合。分段追加颜色链时，较早的直接输出候选同样恢复为可采样目标。
目标池可能短期缓存此前使用的纹理，计划目标数下降不等于保留内存立即下降。

直接显示输出不能再采样，不替换可读的 `PresentSource`。只有成功提交才置当帧显示完成标记；
失败时 Present 使用最近有效输入。标记在帧头、重新编译、resize/shutdown 时重置，
执行诊断以 `Integrated` 说明 Present 已由前驱完成，实际 draw 数保持0。
历史目标禁止直接显示。8x/16x保留独立路径；扩展到其他后端、采样档位或不同输出传递函数前必须重新验证。
`integratePresentation=false` 提供同一场景的画面/成本对照；该描述结构增加字段后调用方须重编译。

`Test_PresentationIntegrationGpu.cpp` 对比独立和直接输出的真实画面、draw 数和目标数，
覆盖所有显示操作、组合、历史输入、FXAA、奇数偏移视口、2x/4x及更高MSAA回退。

`Test_PresentationStages.cpp` 另有20阶段、256帧确定性开关序列，覆盖共享输入与历史目标，
不靠枚举每一对旧/新效果来维护兼容。测试探针从未使用的 View 自动取号。

真实设备另将清单之外的同一种测试Pass挂载0/1/5/10/20次，使用公共准备/提交/清理路径和共享
额外纹理，在两后端验证35种开关/提交失败/消费者组合。每次成功提交反相一次，以独立字节参考
验证整条颜色链；测试不会添加生产效果或要求下游适配新的效果名称。

## 7. 短成本测量

可选目标 `AYRenderer_PresentationPlanBench` 在无设备条件下测0/1/5/10/20阶段的声明、验证
和建图，分别覆盖普通颜色与共享额外输入，并计数普通C++分配。报告路径作为唯一可选参数。
这不包含算法准备、Shader首次创建或真实GPU成本；Debug结果不作为产品运行预算。

Windows 的 `AYRenderer_PresentationGpuBench d3d11|d3d12 report.json [0|2|4 MSAA]`
以1080p/4K、ABBA顺序短测独立与直接显示，记录实际draw、计划目标、CPU及有效GPU时间样本。
每块24帧预热、48帧采样；空Forward场景用于隔离末尾显示成本，不代表游戏整帧性能。
GPU值只采不同的已完成帧；没有时间查询时样本为空，不将0解释为免费。

GPU 回读夹具必须排在被测 producer 后面；直接效果夹具用末端255，避开已前置保留的点计算55。
独立缓存标识期望集中在 `unittest/ShaderIdentityExpectations.h`，避免各历史集成测试复制版本字面量。
