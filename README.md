# AYRenderer

## Camera output CPU frame delivery

After `renderViews`, a host can call `requestCameraOutputReadback` before
`endFrame`, then poll the returned handle in later frames. The result owns a
premultiplied RGBA8 CPU frame and reports its row origin. Three requests and
32 MiB of staging memory are the in-flight limits; use
`discardCameraOutputReadback` if a frame is no longer needed. The Renderer
reserves GPU view 112 for copies, leaving 131 generic UI offscreen views.
The host supplies any video encoding or streaming transport.
`convertTransparentCameraFrame` converts a completed premultiplied readback to
an owned top-left, straight-alpha RGBA8 frame with a host presentation time;
it preserves the destination on invalid input. `writeTransparentVideoFramePng`
can export each normalized frame to a distinct PNG path using a temporary
file and leaves existing output intact. Video encoding and live compositor
transport remain host integrations.

## Transparent 2D camera output

An explicit `CameraRenderView::transparent2DOutput` renders SceneOverlay
Sprite quads or dynamic avatar meshes into a Renderer-owned offscreen output.
The view requires an
RGBA clear of zero and rejects 3D, WorldLit2D and particle items before any
submission. Sprite blending accumulates premultiplied RGB and source-over
Alpha, so the output can be sampled by a later GPU consumer without an opaque
background. The host can use the asynchronous camera output readback above
for video capture; ordinary camera views retain their established blend behavior.
`kPuppetMasked2DPhoskiaSource` multiplies albedo alpha by a second texture's
alpha at the same UV. Rigid Sprite and dynamic mesh draws can use it; content
must align the mask atlas to every selected albedo variant.

## main集成（2026-10-23）

灯光演出与现有云/艺术后处理共存。保留主仓View50..54，GPU点阵计算/读回改用55..59，
体积网格/历史使用240..242；后续透明帧读回占用112，UI离屏单视图预算现为131。FrameGraph保留既有0..36编号，
新增体积资源37..40（Count41），消费方需重编。生产包合并后为78个标准变体。
截图保留按请求唯一文件名的FIFO队列，GPU点阵在设备关闭前释放、读回内存活至设备关闭后；
资源热刷新继续使用按owner登记/撤销的观察者。合并门禁见docs/testing.md。

## 单层体积云（2026-10-23）

主Deferred管线新增Clouds阶段（View54），在体积雾之后、透明物体与曝光/色调映射之前，
用三维密度光线步进、Beer透射、方向光散射和短程光照步进生成云内自遮蔽。
按GBuffer世界位置截断射线，天空射线可以越过场景相机far plane，受独立maxDistance限制。
默认关闭；高级Host使用Renderer::setCloudSettings，普通环境显示使用AYEnvironment桥。
设置为拥有值、owner线程调用；每帧不推进时间，宿主提供shape/detail phase。

CloudQuality控制8..96视线步、0..8光照步、最大距离和composite/transmittance/radiance/ray调试。
连续三维密度取代格状椭球包络；cloudType（0层云/1积云）、clustering、edgeSoftness与coverage独立。
软边缘从固定外边界向内形成密度过渡，薄连接层可表达云块聚集，不靠全屏模糊。
宏观天气场与三维内部侵蚀独立，HG双瓣与衰减补光近似多次散射，短距离加密的光照步进保留云内明暗。
当前为全分辨率主Y向高度云，可叠加独立高空薄云片；无地面云影、物理多次散射求解、地球曲率或历史重投影；
不消费环境historyRevision，TAA不提供云专用速度补偿。Forward与独立renderViews不支持该阶段。
默认程序生成可平铺32³ FBM，种子变化重建约148KiB RGBA8 atlas，其余帧复用。
缺省weatherMap为独立seeded 256²分布图，weatherScale独立控制宏观尺度；天气场固定于世界，
phase推进连续体积噪声。自定义linear atlas支持32³/64³/128³，对应272×136/528×528/2080×1040：
8×4/8×8/16×8切片，每片有效数据加一圈wrapped边框。形状R为基础，GB补充起伏、A预留；细节RGB侵蚀。
灰度仍有效。legacy weatherMap使用R乘全局coverage，不再在较低coverage提前饱和。
spatialWeather模式RGBA分别为覆盖率/云型/降水/相对厚度，weatherOrigin/Extent显式世界映射，
weatherRepeat控制循环；非循环场外无云/无雨，边界夹到纹素中心。该模式必须提供有效线性2D纹理。
场厚度映射0.25..1倍；B同时加强云光学厚度，并在云底至Y=0间形成有限雨幕介质，
雨幕再乘precipitation；深度截断仍保留，Host可用同一数值场控制近处粒子。
句柄借用到endFrame，Host负责加载与释放；失效句柄、sRGB/cube或错误atlas尺寸使本次云提交跳过。
未启用atmosphereEnabled时，关闭、零absorption或主云/高云均无介质时不分配云目标；重新启用无旧帧。
可选天空渐变/空气透视使用horizonColor与aerialDensity，云密度关闭时仍可绘制天空；
只对天空和云积分着色，几何仍用既有DepthHaze，避免前景重复加雾。不是物理Rayleigh/Mie大气求解。
私有噪声/天气图与几何按管线关闭路径清理，输出由FrameGraph瞬态池拥有。
云射线在相机相对空间求逆并归一化，再添加世界平移，避免公里级相机位置与小near组合丢失精度。
授权素材先入AssetRepo，再离线转换复制到构建assets，见[素材流程](../../AYDocs/ASSET-SOURCE-WORKFLOW.md)。

固定View54用于云；合并点阵55..59与体积240..242后最大单View离屏UI绘制为132，分组预览仍可能留空槽。
CPU契约/边框测试为AYRenderer_Clouds；实机独立目标AYRenderer_CloudRenderTest，
参数为fresh-output-directory和d3d11/d3d12，读回检查天空透射、前景深度、空密度/天气图、
自遮蔽、phase变化、关闭和resize。测试记录见docs/testing.md。

CloudForm显式选择Blended(旧cloudType混合)、Cumulus、Stratocumulus、Stratus、Cirrus、Towering。
新形态采用不同高度剖面、额外中尺度结构及方向性薄云密度；不以coverage代替云型。
显式form覆盖cloudType与天气图G的形态选择；R/B/A仍控制覆盖、降水和厚度。
highClouds可独立设置coverage/density/baseHeight/thickness/scale，主云为零仍可显示高云。
高云是带世界空间卷曲细丝的解析薄云片，没有第二套体积步进或自遮蔽；不受主天气图限制。
有主云时须把高云片放在整个主高度层之上，重叠高云在低层API中被跳过。
该阶段用于静态形态对照，尚无形态之间的交叉渐变、天气前沿演化或高云风动画。
CloudQuality.samplingPhase为0..1积分采样偏移，缺省0；宿主可在冻结场景配合通用TAA推进采样序列，
它不移动云、风或天气时间。切换场景/云型须notifyCameraCut；没有AA时保持偏移固定，避免闪烁。
静态展示的多帧收敛不代表已实现运动云的速度补偿或云专用时域重建。

## 区域视图与传送门表面（2026-10-23）

`CameraRenderView::spaceRegionId` 非零时只提取相同区域的 `DrawItem`；零保留旧版
未分区渲染。普通 Entity 3D 相机会把所属区域传给主视图；`CameraViewportDesc`
可覆盖多视图的区域筛选。使用
`createCameraOutput` 和 `createPortalSurfaceMaterial` 可把先渲染的子视图按入口表面
当前屏幕投影取样，斜视时仍跟随观察者位姿。一个输出对应一个材质；先销毁材质，
再释放输出。父子视图共用场景时，资源依赖检查只考虑该视图实际可见的区域。
视图数量和 Forward 渲染域仍受现有多视图契约约束。见
[连接空间说明](../../AYDocs/CONNECTED-SPACES.md)。

## GPU计时的固定View归属（2026-10-22）

现有RenderFrameStats/DebugOverlay汇总全部Bloom金字塔、自动曝光、SSR、Volumetric、
16个Decal View及Transparent的稳定选择mask；不再遗漏这些已实现阶段的非零设备样本。
CPU行来自当前dispatch，GPU值来自已完成backend查询；动态相机/预览/UI离屏不归入相邻主Pass。
View计时可能包含clear/resolve，0可能表示无有效timer，不能当作精确的当前CPU帧或纯kernel预算。
实现未增加新的分析器、设备查询或GPU提交；独立非零fixture见[测试维护](docs/testing.md)。

## 同池UI程序与失败初始化（2026-10-22）

UI后端的flat/SDF程序借用ShaderResourcePool缓存，与内置Pass采用同一归属方式。
关闭一个backend或初始化失败只reset它的token，不全局release共享程序；其他同池backend可继续绘制。
私有white贴图仍释放，索引和全部binding一起清空，重复shutdown及同设备失败重试可用。
池的正常驱逐/shutdown仍可能使借用token失效；这没有新增引用计数、永久驻留或并发关闭保证。
程序、字体图集和私有贴图的拥有者分别保留，Host仍须先关闭UI再销毁Renderer。
真实双backend像素与缺SDF包的部分初始化证据见[测试维护](docs/testing.md)。

## 管线GPU清理边界（2026-10-22）

内部RenderPipelineLifecycle集中管理内置Pass的清理：管线替换/关闭使用完整清理，
窗口尺寸/MSAA重建使用共同的主附件边界。Transparent/选择遮罩、Motion、Decal先于
其借用的scene/GBuffer/Lighting附件释放，reset后暂不render也不会保留旧Decal目标。
清理函数本身保留Pass对象、开关/作者参数和UIPass借用的UI backend；
管线替换随后仍由调用者clear并按描述重建，关闭/resize/MSAA按原策略保留实例。
FrameGraph/目标池和主scene FBO仍由原所有者清理。自定义内部Pass仍负责自身资源。
Host应先停止UI/预览callback并关闭借用Renderer的UI backend，再替换设备上下文或销毁Renderer。
编译/实机门禁记录见[测试维护](docs/testing.md)；这不提供全局GPU引用计数或异步退休协议。

## Decal 目标的设备接纳与附件归属（2026-10-22）

DeferredDecal 的颜色目标和借用深度的 FBO 均经过内部 BGFXAdapter 创建/释放。
在释放旧目标之前查询尺寸、格式和设备支持；无效描述保留旧目标。
借用附件 helper 拒绝空列表、数量越界、无效/重复句柄及设备不兼容组合，
创建的 FBO 不拥有附件；调用者负责同设备有效句柄与寿命。
这不提供外来句柄鉴权、全局引用计数或 GPU OOM 恢复；实际分配失败仍回收部分目标并回退原 GBuffer。
实机的附件释放/再次绘制及 Decal 像素验收见 [测试维护](docs/testing.md)。

## 多行UI文本（2026-10-22）

UIRenderBackend的wrapped文本在塑形前按LF/CRLF/CR和Unicode行/段分隔符分段，保留空行和尾部空行。
正宽度度量或styled wrapToBounds进入同一段落域，行间距、逐行水平对齐和整块垂直对齐一致。
默认无约束的单行路径保持兼容；outline和offset shadow可用，shadow blur仍未实现。
空段不会上传分隔符字形。源级独立分段、真实Noop度量/批次及两后端逐行像素参考见
[测试维护](docs/testing.md)。此实现是cluster级贪心换行，不是完整Unicode断行或每软行重新塑形。

当前结构与源码定位见 [架构入口](docs/architecture.md)；后续工作见
[根渲染路线](../../AYDocs/AYRendering-Architecture-Roadmap.md)与
[2026-10-22 源码自审](../../AYDocs/RENDERER-ARCHITECTURE-AUDIT-20261022.md)。
下方带日期的功能增量说明保留；早期 R1–R5 表格已明确标为历史记录。

## 资源驻留观察（2026-10-22）

高级Host持有Renderer时，可在资源提交线程显式调用`resourceResidencySnapshot(maxRows)`，
配合`AYRenderer/ResourceResidency.h`取得拥有自身字符串和值的报告，不加载或保留GPU资源。
Mesh/Material/Texture表总计覆盖当前RRM；纹理行最多512、按id排序，每行最多4个1024字节缓存key。
Mesh总计另含当前成功版本的顶点、索引及可选线框缓冲上传长度；这是逻辑字节，不是驱动分配。

高级 2D 宿主可用 `createDynamicMesh2D` 创建固定三角拓扑的 SceneOverlay 网格，
再用 `updateDynamicMesh2D` 逐帧替换全部局部 XY 顶点和最终 UV；更新保留 MeshHandle、
索引和材质。顶点数为 3..4096，索引数不超过 24576；无效数据或顶点数变化会拒绝，
不更改已提交网格。它只由 Forward2D overlay 绘制，不进入 3D/WorldLit2D 路径。
参数化角色可把 `evaluateMesh` 的顶点转换成 `DynamicVertex2D` 上传，并在 RenderScene
提交带 `DrawPayload2D` 的 DrawItem；宿主仍管理层实体、GPU 贴图及网格销毁。
高级宿主也可用 `observeMeshLogicalPayload` 或 `observeTextureLogicalPayload` 按当前
句柄读取一项逻辑载荷与资源上下文 ID；缺失句柄会清空输出并返回 false。
借用纹理的逻辑字节保持未知。单项值仍不能充当实际显存租约或硬预算上界。
`maxRows=0`只求总计；行遗漏、key截断与未知描述分别显示。

逻辑存储、当前创建负载、来源像素/块负载、保留CPU图像分开计量；GPU拒绝重载保持旧版本描述。
borrowed格式未知且排除自有存储，dynamic初始上传为0而不累计后续更新；cube六面与CPU副本独立计量。
采样值表示创建默认值。此报告不涵盖Pass/目标池/device helper、退休对象或驱动开销，不是全设备显存。
其拥有值可在shutdown后阅读，opaque id不能用来恢复GPU对象，也不提供并发访问保证。
编辑器通过现有Resource调试分类按需采集24行和摘要。范围与验收见
[设计](design.md)、[测试维护](docs/testing.md)和[跨模块边界](../../AYDocs/RENDER-RESOURCE-OBSERVATION-20261022.md)。

## 作者资源刷新确认（2026-10-22）

Renderer通过Resource通用报告回调确认已驻留Mesh/Material及linear/sRGB纹理变体。
成功替换返回Updated和稳定id/当前上下文，拒绝返回Retained并保留原GPU；
未驻留不为生成报告而强制加载，未知类型NotHandled。纹理诊断区分CPU准备拒绝与设备创建拒绝。
结果不授予GPU租约，也不把多个资源/颜色变体变成整体原子事务。

回调捕获稳定Impl并跟随Renderer移动；关闭前禁用旧alive token，只清理自己的注册token，
不移除后来安装的Host回调。move assignment先关闭原目标；所有操作仍要求submission owner线程。
首批Noop桥接23例/444断言、两后端逐层实机各1例/1456断言及实际Editor拒绝/修复通过。
完整Renderer CPU1006例/17264断言、两后端Gpu各53例/44644断言与真实Editor/Entity门禁通过。
最新Editor来源隔离另行验收，最终以[测试维护](docs/testing.md)的阶段结果为准。

## 与 GPU 一致的网格提交元数据（2026-10-22）

资源网格上传先准备不可变`MeshSubmissionInfo`，随GPU buffers共同入表和替换：范围、材质slot、
局部中心及骨骼palette来自同一成功上传版本。非法输入拒绝时保留旧GPU和旧快照；L2-only卸载不改变L3。
快照不持有完整L2顶点/索引，不延长GPU寿命。Raw/procedural mesh仍可没有资产元数据。

高级提取器通过`meshSubmissionInfo`查询；`resourceContextId`区分关闭重建/替换后的资源上下文，
`isMaterialResident`仅查当前材质表。缓存须先核对上下文；这些查询不是foreign-id校验、shader就绪证明或GPU lease。
游戏侧仍使用MeshComponent和已安装的RenderIntegration。准备接纳预算和实际验收见
[设计](design.md)、[测试维护](docs/testing.md)与[跨模块设计](../../AYDocs/RENDER-MESH-SUBMISSION-METADATA-RFC-20261022.md)。
直接网格上传在读取输入数据前以 64 位中间值核对顶点、索引及可选线框缓冲
的字节数，超出 adapter 的 32 位上传范围返回无效句柄，不创建半份 GPU 网格。

## Cube 与渲染目标创建接纳（2026-10-22）

原始六面RGBA8 cube上传在查缓存前拒绝不可表示尺寸/超256 MiB负载，Noop不支持该入口。
设备尺寸、合法格式和flags通过后才复制留存CPU六面数据，入表使用move；合法数据仍按六面/完整mip控制采样。
cube、单色/带深度/GBuffer/depth-only与blit目标统一检查设备尺寸和枚举，
颜色深度helper保留首选格式的合法回退，不会把深度候选当作第二个颜色附件。
带深度FBO直接走其专用创建路径，避免先分配再销毁一张不用的颜色目标。
这些是创建接纳与失败回收边界，不是任意更新矩形校验、全局VRAM预算或设备OOM恢复。

## 纹理层级与采样（2026-10-22）

资源桥上传完整 mip 链；单层保持单层；合法前缀链明确回退基础层，仍校验所有尾部。
`.txr` v2 保存过滤/寻址，v1 读取采用历史默认值；作者通过导入设置 Apply 显式发布和刷新。
CPU 修改 sampler 不会自动改动已驻留 GPU 纹理。准备总字节上限256 MiB，不生成缺失层级。
ITexture桥的sRGB取调用端请求与资源标记的或；传false不强制覆盖资源标记为linear。
RGB8/BC4/BC5 的 sRGB 请求逐级展开 RGBA8；非4倍数尺寸的压缩纹理也展开，保持精确尺寸和层级。
其他接受的压缩数据保留原编码；BC4/BC5 保持 R/RG、缺失颜色0、alpha1，不隐式重建法线 Z。
过滤及 Repeat/Clamp/Mirror 到达 GPU 创建端；各向异性>1采用设备最大值，Border用Clamp、LOD bias用0并诊断。
Adapter 在分配前校验设备格式、标志和精确字节数；非法请求返回无效句柄，失败重载保留旧 GPU 数据。
CPU、真实两后端逐层/分数LOD/寻址/重载检查见 [测试说明](docs/testing.md)；
`AYRenderer_TextureSamplingRenderTest <新目录> d3d11|d3d12 [checker|stripes]` 导出固定相机画面和独立计时。
夹具仅反映所选图案、相机和设备；上传字节不等于驱动实际分配量。

## 重载失败保留既有 GPU 资源（2026-10-21）

网格及贴图重载先上传独立临时资源，成功后在原句柄下交换 GPU 数据，再销毁旧数据。
准备失败保留原路径缓存、GPU 缓冲和已有引用；线性与 sRGB 贴图分别处理。
RenderAssetBridgeTests 用无效驻留输入验证拒绝、旧资源保留与修复重试；这不是 GPU 内存耗尽测试。
Editor 真实 D3D11/D3D12 重导入测试另检查几何、材质和贴图的可见更新与稳定句柄。

## Scene 灯光快照（2026-10-21）

高级 Host 可用 `RenderScene::setSceneLights` 为提交包复制最多8盏灯的不可变值快照。
包副本共享拥有的数据；原输入、World 或另一个包销毁后快照仍有效。
没有快照时保留既有 Host 灯光；存在且 count=0 时关闭直接照明，不移除环境或自发光。
`clearSceneLights` 恢复 Host 策略。过滤/多视图派生包保留快照，显式预览灯光设置优先。

游戏侧通过 AYEntity 的 `LightComponent` 和已安装的 RenderIntegration 提取，不逐帧调用
Renderer setter。Directional / Point / Spot 可编辑；Point 阴影仍不支持，阴影总开关和图集预算不变。
CPU 所有权、空灯光与旧默认照明测试位于 Contracts / LightingShadow 组；真实两后端灯光创作
和阴影读回证据位于 AYEditor 的 `AYEditor_LightAuthoringRenderTest`。

## 作者材质的多资源根着色器解析（2026-10-21）

材质 shader 先按原相邻路径解析，再依挂载顺序检查所有资源根。
旧 `shaders/name.phoskia` 引用仍兼容各根的 flat basename。
这修复项目/派生根排在运行时 shader 根前面时，导入材质找不到 shader 的问题。
RenderAssetBridgeTests 覆盖第二个根和旧 flat 回退；真实导入材质的 D3D11/D3D12 流程另在 Editor 验证。

## PBR 材质与可编辑内容（2026-10-20）

不透明 Deferred 材质可选 `MaterialModel::ToonLit`（持久材质参数
`__ayMaterialModel=2`）。每盏灯的直接光按法线夹角分为四级，再保留原灯色、
遮挡、衰减、PBR 环境光与自发光；同一 Scene 可混用 StandardLit、Unlit 和 ToonLit。
Forward 与透明路径仍按已有材质 shader 绘制，不承诺相同分段效果。
旧材质的模型值 0/1 与缺省 PBR 结果保持不变。

不透明 Deferred 材质还可选 `MaterialModel::SoftDiffuseLit`（持久材质参数
`__ayMaterialModel=3`）。它只给每盏灯的漫反射增加随 N·L 减小而增强的有限补光，
保留原 PBR 高光、灯色、阴影、衰减、环境与自发光；适合需要柔和近景受光的材质。
这是固定形状的局部近似，不模拟皮肤次表面散射、厚度透射或头发纤维。
Forward、透明和自定义 shader 不切换到该模型。旧材质仍使用原模型和结果。

`PbrSurfaceSettings` 统一 Forward/Deferred 的 UV 缩放/偏移、法线强度和粗糙度偏置，
默认值保留旧材质；使用现有 `.mat` 通用参数，不扩 IMaterial ABI 或 GBuffer 附件。
Forward 法线改为 inverse-transpose，内置 PBR 的 Mask cutoff 与 Deferred 对齐。
高级 Host 可创建独立资源材质快照，供 Editor 预览，不改路径缓存里的 Scene 材质。
`PreviewSceneSettings` 提供独立灯光与可选 HDR 显示，复用主 PostProcess shader，
与 retained UI 共用 view/内存预算；相机非有限值在提交前拒绝。

[Material Workshop](demo/MaterialWorkshop/README.md) 包含十七种程序化表面、四种状态、
主场景、参数/剪裁/形状/透明研究及近景，共 128 份原生材质、411 张贴图和 104 个可编辑 Scene。
`AYRenderer_MaterialTool` 使用资源模块编码器，`AYRenderer_MaterialDemo` 真正加载材质文件。
可用生成器重建、使用 `AYEditor_MaterialSampleTool` 写网格/场景工程，再在 Editor 中编辑。
精选与连续过程导出使用 `demo/ExportMaterialShowcase.ps1`。
Native 展示以共用直射灯为对照，默认关闭阴影、TAA 和环境 cube；不声称完整 IBL 等价。
材质实例共享编译程序，销毁快照只在没有其他材质使用该程序时才释放池令牌。
路径材质重载先准备新 GPU 实例，获取失败时保留原缓存和稳定句柄。
StandardLit 资源桥为省略的旧参数建立实例缺省值：金属0、粗糙度0.5、AO/opacity/normal-Y为1、
发光为0，保留显式参数；避免稀疏材质继承其他 draw 或预览留下的 program uniform 值。

## 色板动态与生产构建复用（2026-10-19）

`--animate-palette`以3.2秒循环在原始、暖色、冷色三个实际色板之间平滑插值；
默认Sunset，也可搭配启用FixedPalette的`--look`。它改变颜色值而非仅排列最近色列表。
交互模式Space冻结时间，恢复时从暂停点继续，F切换管线；动态批量导出冻结几何，以区分颜色变化与物体运动：

```powershell
AYRenderer_ColorStylizationDemo --motion-gallery --animate-palette --hidden --scene observatory --look demo/looks/reference-palette.rsl --output palette-performance
```

该配方只在demo Host推进既有值配置，不引入新的运行时动画API、look字段或GPU历史。
另附PaperEcho和PhosphorStop两个独立组合作者look，含网点/双色/残影及固定色板/停格/显示模拟；
选择与使用方法见[作品集](demo/looks/README.md)。内置preset仍18项。
`demo/ExportStyleShowcase.ps1`以全新目录导出六章精选和可选动态内容，附实际look快照及摘要清单。

生产shader生成增加本地内容复用：输入指纹相同后仍用`AYRenderer_ShaderBuild <platform> <profile> <package> --check`
验证完整目录、目标、源码/define覆盖和阶段字节摘要，再复用现有包。编译器/AYShader库、生成工具源码、
构建脚本、include内容或目录变动会失效；缺包、损坏或覆盖不足回到完整70变体构建。
私有`.inputs`状态位于包目录之外，不随安装/发布；包协议与严格运行时加载保持原样。
这是可信本地构建复用，尚无逐变体持久缓存、共享缓存认证或并发源码编辑快照协议。

## 保色明暗分层（2026-10-19）

ValueBands对max(R,G,B)作2..32阶量化，按统一增益缩放RGB，保留非黑色的通道比例；
支持既有像素采样/Bayer/印刷组合，alpha保留，无新增Pass或目标。关闭量化和其他颜色组时旁路。
CelInk/CelPaint分别提供硬朗轮廓和较柔和绘画起点，默认展示曲面材质；共18套预设。
这是显示颜色处理，不是替换PBR光照。RSL写schema4并读完整1..4；旧版本的映射枚举仍限定0..2。
原有v1/v2/v3样例保留兼容输入，新Cel样例为v4；preset count由公开常量统一，Editor布局ABI37不变。

## 曲面与建筑展示内容（2026-10-19）

画廊追加天文仪和运河街区，含球体、圆柱、圆环、坡屋顶、程序纹理地球、金属/非金属粗糙度排布。
`--scene observatory`或`--scene canal`固定场景；Tab在六种内容间切换，切换清除相机/艺术历史。
同样支持静态/动态导出、任意look和两条管线；原有auto场景选择保留。
所有几何由该高级Host演示持有，显式创建和销毁，不增加玩法API或自动Scene导入格式。
Windows画廊/离线工具从wmain接收UTF-16并转换UTF-8，支持参考图与作品路径中的中文/emoji。

## 参考图色板创作（2026-10-19）

`StylizationPalette.h`提供无GPU的RGBA8取色，alpha加权、5-bit直方图和weighted median cut，
最多16 Mi像素、2..16色；输出可以少于请求，单一bin复制为两色。失败不替换输出。
其颜色空间与FixedPalette显示RGB匹配，不是感知均匀量化或跨平台逐位确定性承诺。
`AYRenderer_RenderStyleTool palette <image> <output.rsl> [colors]`生成独立look；
`inspect <look.rsl>`读/迁移后打印规范配置。该工具不初始化设备，也不需要production shader目录。
JSON数值按Float32控件读取，允许`0.12`这样的短小数；仍拒绝未知字段、错误类型和越界配置。
`demo/looks/palette-reference.png`和同名可编辑SVG为32色参考图，标准库脚本
`demo/GeneratePaletteReference.py`可重建；`reference-palette.rsl`是它生成的8色色板。
`AYRenderer_ColorStylizationDemo --gallery --look <file.rsl> --hidden --output <dir>`导出自定义look；
也可加`--motion-gallery`生成单look的两管线动态序列。

## 时间风格与动态内容（2026-10-19）

新增Afterimage、StopMotion、TileRefresh和TimeEcho起点，左右箭头遍历十八个look。
TemporalFeedback独立处理残影混合、整帧停格和有界分块刷新，可与色板、印刷和显示效果组合。
保持周期按成功主视图提交计数，1..120帧；分块4..128物理像素，每块每周期更新一次。
两个RGBA8历史按视口尺寸独占；切镜、尺寸/原点变化、失败输入后从当前帧重启；禁用释放。
`resetStylizationHistory()`只重置艺术历史；镜头跳转使用`notifyCameraCut()`处理全部历史。
历史偏移超出视口时使用当前像素，不在窗口其他区域采样；没有运动补偿或持久内容缓存。

`--motion-gallery --hidden --output <directory>`输出两管线的原画和四种时间效果，每段32张、
固定1/30秒推进、每3帧取样的3.2秒序列。一次F9截图即可，PNG在GPU回调完成后的endFrame生成，
shutdown做最终收尾；请求队列上限32。

## 主视口背景与着色路径一致性（2026-10-19 修正）

`beginFrame(clear)` / `beginCompositeFrame(clear,...)` 的 clear RGBA 现在传递给主场景。
RGB 为 scene-linear，finite/clamp 到0..1并遵循既有RGBA8清屏量化，再进入 tone-map/gamma
与颜色风格化。Forward 不再覆盖为硬编码灰色；Deferred 无天空时不再写黑色/透明背景。
有效天空覆盖背景；SceneColor alpha 表示合成图像透明度，几何 coverage 仍由 GBuffer RT3.a 提供。
Composite 每帧显式把 view0 绑定回 backbuffer，视口外窗口清屏不接受后处理。

Opaque Forward 与 Transparent 共用 SceneLighting 灯光数组、阴影和可用环境绑定；
旧 SimpleLit 自定义 shader 继续接收单灯 uniform。内置 PBR 的无IBL环境项与 Deferred 对齐。
Forward 自定义光照算法和 Deferred 统一 PBR 并不等价；画廊统一使用内置 PBR、同灯光和曝光。
其对照验收同时检查两条管线的背景/alpha、视口边界和同材质受光，而非只比较 D3D11/12。

## 风格化创作工具箱（2026-10-19）

`setColorStylizationSettings` 复制并清洗完整look，状态跨重建/resize保留；默认关闭。
主视图顺序为 AA → StylizedOutline → ColorGrading → ColorStylization → TemporalFeedback → AnalogDisplay → Present → Editor/UI。
轮廓与颜色有独立gate和输出，关闭时不声明节点/目标，不做identity draw。
独立renderViews/preview不运行该链；所有颜色处理包含已合成的主视口背景，窗口外部与UI不参与。
作品读写与参考图输入接受UTF-8路径；保存使用AYIO明确的UTF-8原子入口并创建父目录。

| 组 | 能力与边界 |
|---|---|
| Pixel/RGB | 1..64物理像素中心采样，RGB每通道2..32阶，虚拟像素Bayer4；原有预设保留 |
| Mapping | 亮度双色；2..16色自定义固定色板，显示RGB加权距离最近色，平局选首项；不宣称感知均匀色空间 |
| Print | 网点/线屏/交叉排线；物理像素间距2..64，角度、tone gamma、混合强度、独立纸墨RGB |
| Outline | 亮度边缘、相对view-depth轮廓、Deferred法线折痕；Forward DepthNormal使用depth，缺少几何输入时旁路 |

颜色内部顺序：像素中心 → Bayer/颜色映射 → 印刷。轮廓在这之前合成，因而可接受分级、色板与印刷。
颜色、轮廓及显示阶段保留采样alpha，时间反馈对RGBA一同混合；其他阶段保留输入alpha；geometry coverage不裁切整幅颜色效果。轮廓只读取可用不透明深度/法线，
透明表面、内部隐藏线和超出视口的内容不能由这个屏幕空间效果恢复。
PixelRetro/LowColor/DitherRetro之外增加Pocket、Sunset、Noir、Comic、Engraving、Blueprint。
Blueprint使用共有的depth模式；法线折痕需要显式选择，避免预设暗中引入Forward/Deferred差异。

```cpp
auto look = ayt::render::colorStylizationPreset(ayt::render::ColorStylizationPreset::Sunset);
look.pixelSize = 4;
renderer.setColorStylizationSettings(look);
// Explicit host IO; .rsl is experimental and not automatically cooked/packed.
std::string error;
ayt::render::saveColorStylizationLook("sunset.rsl", look, &error);
```

包含 `AYRenderer/ColorStylizationLook.h` 使用版本4 `.rsl`配置（兼容完整1/2/3，旧组默认关闭）：64 KiB上限，完整字段、严格类型/
范围/版本检查，失败不替换输出；保存使用AYIO原子替换。没有路径引用或GPU对象，不自动接入
ResourceManager/Cook/Pak。Editor Render面板可编辑参数与hex色板、保存/加载项目内look、重置；
用户偏好schema8保留完整控制，作品文件可独立共享，不改变Scene序列化。

Windows `AYRenderer_ColorStylizationDemo` 含彩色展台、Noir城市、动作角色和机械工坊，无外部素材。
0..9切换look，F切换管线，Space冻结，G切换分级，F9截图。
`--gallery --hidden --output <directory>` 导出两管线各十八张；`--d3d12`切换后端；
`--write-presets`写出十八个`.rsl`，`--look <file.rsl>`加载自定义配置。
离线catalog包括实际四个阶段的shader。验证入口见[测试说明](docs/testing.md)。
## 主呈现文档与缓存生命周期（2026-10-20）

高级宿主可用 RendererSubSystem::setMainPresentationForOwner 临时替代 World 与独立
相机视图，在主管线提交一次 RenderScene，包含完整 Deferred 扩展灯。匹配 owner 的
clear、owner 替换和 shutdown 恢复捕获的相机、灯包、雾/密度绑定、天空、镜头/曝光、
后处理开关、编辑器叠加、管线与视口并使时域历史失效；无关 owner 的 clear 不生效。
提交线程内使用，回调期间递归 set/clear 被忽略，不创建额外渲染帧。
这是文档宿主接缝，独立 CameraRenderView 仍保持 Forward 契约。

EmissivePointCloud/FixtureEmitters/FixtureHousings 的 releaseResources 可显式回收
同一存活 Renderer 上的缓存。先撤下所有借用/保留 draw packet，丢弃共享句柄的 facade 副本；
调用幂等，选项可复用，设备重建仍须重建 facade。GPU诊断 host packets 继续异步托管。


## 聚光灯数组与 HDR 发光点（2026-10-17）

主Deferred的setPerformanceLights复制最多256个Spot，表面/体积/阴影独立预算，
锥体AABB/scissor、GGX与五种程序化投影。体积光消光一次，再累计各灯散射。
有限出光口与核心/外场两半角控制发散；AABB粗裁剪后解析裁剪到真实前向光束弦段，
避免用稀疏包围盒采样产生悬空亮片。表面与体积共用同一角度/出光口轮廓。
默认预算64/16/4，上限256/64/8，不代表帧率保证。
apertureRadius默认零兼容点源，节目新默认使用有限半径；阴影仍是点源投影近似。
patternFocus[0,1]柔化程序化图案边缘及贴图采样，iris(0,1]裁剪投影口径；
外部Texture Gobo由Renderer拥有有效纹理句柄，表面与体积共用旋转/缩放/光阑。
失效或已销毁的句柄在setPerformanceLights处拒绝，原灯包保留。
shadowSoftness[0,4]以阴影图像素指定可选2x2滤波半径；零保持旧硬阴影，
此项只柔化遮挡边缘，不模拟面积光几何半影。
局部雾盒靠边缘四分之一半轴距离平滑降低密度，避免光束在空气中被硬截断。
VolumetricSettings可选noiseStrength/noiseScale/noiseTime让介质密度缓慢起伏；默认强度零
保留原静态介质。全局与局部消光、每束散射共用相同的变化密度。
EmissivePointCloud提供最多65536个逐点HDR色发光点、亚像素覆盖及共享材质批次。
maxPoints限制可见点数，超额时按稳定ID选取，与输入顺序无关。支持硬件时默认
逐点GPU实例化，共享单位四边形；不支持实例化时回退逐帧CPU四顶点，
也可用preferInstancing=false显式对照。两路径使用同一裁剪、半径和能量校正。
GPU瞬态实例缓冲不足时该批按既有渲染契约跳过；仍无GPU端裁剪或间接绘制。
Host入口/生命周期/限制见[演出系统](../../AYDocs/LIGHTING-SHOW-SYSTEM.md)。

## 相机视口与输出（2026-10-14）

RendererSubSystem::setCameraViewports 配置最多16个独立 Forward 视图，按 viewportId
先求相机再提取场景。Renderer::renderViews 提交完整验证后的窗口/离屏 batch；
createCameraOutput/cameraOutputTexture/destroyCameraOutput 管理可采样颜色和深度附件。
3D DrawItem.cameraLayerMask 独立于物理 mask，主视图和独立视图都支持镜头过滤。
独立视图不使用主视图的 Deferred、TAA/Motion/曝光历史或编辑器选中视图；WorldLit2D
在显式 Forward batch 中拒绝。详见 [相机系统](../../AYDocs/CAMERA-SYSTEM.md)。
CPU/Noop 用例 CameraViews；独立 D3D11 读回用例 CameraViewsGpu。
离屏 UI/场景预览跳过固定场景和相机 GPU view，合并后单Pass离屏预算为132。


## Deferred Decal v1（2026-10-06）

2026-10-19：主 Deferred 使用非抖动相机和两像素边界，对投影盒作保守视锥剔除。
只提交可能影响当前视口的贴花，保留源列表的叠加顺序；全屏外时不加入贴花图节点，
冷启动也不创建私有 MRT。已有目标在屏外期间保留，重新入镜复用；关闭/空列表仍释放。
`setFrustumCullingEnabled(false)` 同时关闭几何和贴花剔除，可作画面/性能对照。
计数压缩后重新选择 ping-pong 目标；仍校验整个源列表的纹理，不能用剔除掩盖缺资源。
可见贴花仍逐项全屏复制，没有屏幕矩形裁剪、tile/atlas 或透明接收者支持。
定向 CPU、GPU 图像及固定场景性能入口见 [测试说明](docs/testing.md)。

GBuffer 后、Motion/SSAO/Lighting 前的投影贴花；`setDeferredDecals` 复制最多16项，
`setDeferredDecalsEnabled` 默认关闭。localToWorld 是正尺度、正交基仿射矩阵；
local [-1,1] 盒裁剪，+Z 面向接收表面，XY 为 UV。无纹理时也可投纯色贴花。
支持 optional sRGB albedo/alpha、linear tangent normal，以及 roughness/metallic 混合。
保留世界位置、AO/模型ID、emissive/coverage 和原深度；稳定列表顺序决定重叠结果。
两组全尺寸 MRT 交替读写、借用原深度；缺资源不发布部分结果；禁用时下一帧释放私有目标。
Editor 提供明确标注 test projector 的地面预览及 opacity，不是 ECS 创作/场景保存功能。
首版是正确性优先的全屏复制，每项一次draw；全尺寸两组颜色约40 bytes/pixel，
大量可见贴花尚无tile/scissor/atlas优化。仅不透明/切孔接收，不改轮廓、深度或透明材质。

## 体积光／雾 v1（2026-10-06）

Deferred HDR：Lighting → SSR → DepthHaze → Volumetric → Transparent → Bloom。
`setVolumetricSettings` 默认关闭；Render 面板提供开关、密度和 Final/透射率/散射诊断。
8..64 次确定性中点积分，Beer–Lambert 消光 + HG 单次散射；一个按原场景索引选择的
方向光／点光／Spot，方向光 CSM/Spot 读取本帧阴影；点光全向阴影仍不支持。
全局或局部轴对齐盒介质，颜色为散射 albedo tint；天空参与积分，零密度无目标/绘制。
当前支持可选低分辨率积分、froxel与独立系数历史（见后文），无流体模拟或多重散射；
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

## 早期 R1–R5 开发状态（历史记录）

本节及随后2026-07-21产品定位记录当时阶段，不定义当前能力和默认管线。
当前已经有Deferred、完整Bloom链、共享DrawList、刚体实例化和统一UI命令；以本页顶部更新及架构入口为准。

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

## 2026-07-21 产品定位（历史快照）

**当时可用：** Editor / Demo 的前向场景 + UI 合成 + 资源加载 + 蒙皮（bone UBO 已 wire,在 FO 与 ShadowPass 两端均 ship）+ 真 bloom/exposure/tonemap post-process + **带 skinned caster 的方向光 shadow**（硬边,bias acne 可调）。

**当时尚未覆盖（不是当前待办表）：** 延迟渲染、复杂后处理（真 bloom chain / DOF / SSR）、产品级 PCF / VSM shadow 滤波。

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

预设与参数清洗实现位于`src/ColorStylizationSettings.cpp`，公共头只保留值类型与声明。
调整作品配色不再迫使每个Renderer/Editor消费方重编；字段或ABI变更仍需完整重编。

## 显示器模拟补充（2026-10-19）

`look.display`独立控制扫描线、RGB荧光列mask、0..4像素色差、0..0.25曲率、暗角和0..0.25颗粒。
总enabled与display.enabled同时开启后生效；六项全零也是无draw旁路。独立slot27/view52在颜色/印刷后，
输出FG DisplayColor=33；艺术色板结果先完成再接受屏幕变换。没有持久history或噪声纹理；transient输出由FrameGraph分配。mask基于物理视口，
颗粒使用可复用的`ayScreenNoise`有界程序散列与uint16种子。可由Host推进seed；不声称blue noise或VHS。
Alpha跟随warp采样，扫描线/mask/颗粒/黑色边框RGB不调制alpha。独立renderViews仍不执行主链。
Arcade/Broadcast为新增预设，方向键循环十二套；可直接载入`demo/looks/arcade.rsl`与`broadcast.rsl`。
`.rsl`写schema2，完整schema1继续读取，默认关闭display；旧样例0..9保持schema1作为兼容资料。
`PresentationStage`只集中当前帧输入、目标无别名与提交后提升的共同安全边界，效果算法/资源仍各自所有。
离线目录69项，固定view65项，UI单Pass预算142。
## 2026-10-18 聚合点阵表面光与变焦光通

`buildEmissivePointLights`（EmissivePointCloud.h）把最多65536个有限HDR点按世界网格合并，
稳定ID决定累加顺序，线性RGB功率相加，亮度加权求中心，范围为配置range加本单元最大偏移
并限于10000。cellSize[.01,10000]、range[.01,1000]、intensityScale[0,100]；
最多64个候选灯使用有界堆，亮单元优先、相同亮度按单元坐标排序；输入排序与视锥预算不改变结果。
已按稳定ID排序的点阵以哈希网格平均O(N)聚合；任意顺序输入先按ID排序，O(N log N)。
点到单元映射与稀疏累加器为O(N)，候选灯临时存储O(maxLights)，没有逐点SceneLight。
超过1e9的单元RGB分量夹取，clampedCells显式报告；非法数据/重复ID不替换输出及统计。

setPerformanceLights现在接收Spot和仅表面Point，仍共享256项/表面预算；Point要求
volumetric=0、Open、无阴影且constantFlux关闭，使用球形范围AABB/scissor和同一GGX通道，
不扩大旧八灯ABI，不产生体积光。EmissivePointCloud本身仍只提交可见发光；聚合需宿主显式接入。
这不是逐点GI，不能表达遮挡或网格跨界的连续变化；预算排名变化也可跳变。

PerformanceLight.constantFlux默认关闭，开启时performanceLightFluxScale按
(2-cos(2°)-cos(3°))/(2-coneCosInner-coneCosOuter)缩放表面和体积强度。
这是smoothstep角度轮廓的解析归一化；Gobo/iris仍损失光通，有限口径近场不是物理光学。

## 2026-10-19 可见灯具发光口

`FixtureEmitters.h`提供显式的`FixtureEmitters::append`，输入与主视图相同的
PerformanceLight灯包、view/projection和viewport。`buildFixtureEmitterBatch`是可离线测试的
准备函数，最多256灯/1024顶点；非法灯包、相机和设置不替换输出/统计。
仅Spot且apertureRadius>0、intensity>0生成圆形平面，随灯具轴朝向，背面不出光。
Point和旧版零口径不自动造灯泡。发光口按iris收缩；核心/外场余弦轮廓和constantFlux与灯束一致，
可选offAxisGlow提供很弱的前半球镜片辉光，不在背面添加屏幕贴片。

近似辐亮度为intensity/出光口面积×radianceScale×角度权重，保留线性HDR颜色；
不对已分辨发光面再施加平方反比。默认.75px最小足迹按面积修正能量，最大RGB默认10000，
允许[1,60000]，超限在介质透射和足迹能量修正前夹取并计入clamped，避免高亮饱和吞掉雾衰减。radianceScale[0,100]、
minimumPixelRadius[0,4]、offAxisGlow[0,1]均为显式呈现参数，尚非标准光度单位标定。

资源复用现有ParticleUnlit生产材质，圆盘纹理64×64，最多一个CPU批次/绘制；
Additive、深度测试、不写深度，服从不透明场景的完整/部分遮挡，再进入现有Bloom/曝光链。
因Transparent在体积之后，发光口自行读取Renderer的VolumetricSettings：均匀介质解析透射，
局部盒/动态密度用最多16次中心视线积分，使用相同噪声时间与边缘淡出。该近似不做逐像素介质积分。
未生成独立屏幕眩光，不添加第二次散射；现有体积HG方向约定保持不变。

Renderer拥有纹理/材质/quad，借用批次活到下一次append/销毁，保留帧使用OwnedCpu；
Renderer重启后重建Facade。prepared仅为CPU候选数，GPU裁剪/深度及瞬态缓冲可减少实际像素。
尚无灯体外壳、Gobo在镜片上的真实像、镜头鬼影、星芒或物理焦面。


## 2026-10-19 光度与IES

PerformanceLight.intensityUnit支持Legacy/Candela/Lumen；Legacy保留历史表现，物理模式以米为世界尺度，
颜色按Rec.709线性亮度归一化（黑色无光），表面/体积/可见灯口共用performanceLightIntensity。
Lumen以smoothstep光锥与峰值归一化IES的有效立体角换算，iris/Gobo是后续吸收；
Candela直接指定峰值方向强度。constantFlux只对Legacy生效。有限口径近场、range截断和RGB色谱仍是近似。
illuminanceLux提供无截断无遮挡的cd×cos/r²诊断，不把显示器像素宣称为测光仪。

IesProfile::parse只接受LM-63-1995/2002/2019、Type C、TILT=NONE、ballast字段均为1；
最多4MiB/262144测量样本，严格递增角度，支持旋转/90°/180°/360°对称，360°接缝需一致。
不支持Type A/B、外部/内嵌TILT或非单位ballast；失败保留旧对象。颜色不由IES指定。
createIesTexture上传257×129线性RGBA8角度图，CPU灯口与GPU表面/体积共享坐标基；
8bit峰值归一化会量化极弱旁瓣。Host配对不可变profile与存活atlas，Renderer检查句柄后复制灯包。
有效立体角以128点余弦空间求积，水平角均值由原始采样分段线性积分；极窄IES尖峰需提高采样质量。


## 2026-10-19 距离焦平面与棱镜

focusDistance>0使用有限出光口/iris作为几何瞳孔，距焦面z处的弥散圆半径为
apertureRadius×iris×abs(1-z/focusDistance)，除以该处光束半径得到投影滤波足迹。
焦面处额外模糊为零，零focusDistance精确保留旧柔焦；图案使用有界五点瞳孔滤波，
不是衍射/像差/连续光学卷积。patternFocus仍是独立的作者柔化参数。

prismCount为1（禁用）或2..8，prismAngle[0,.35]rad，prismRotation为连续弧度。
每束保持原核心/外场发散，绕主轴倾斜并按束数分配强度；表面/烟雾/可见灯口共享方向与IES。
有限瞳孔的分束在出光口附近允许真实几何重叠；不人为挖空中心。
CPU裁剪取所有分束包围盒并集，体积解析各束交段的保守并集，再对实际每束求值。
阴影仍同一出光位置，投影外场扩大覆盖分束；非面积光阴影。
同灯仍一个表面和一个体积draw，shader最多8束，体积保留最多64步；稀疏宽分束可能欠采样，
不保证与单束相同GPU成本。五条固定Star图案射线展开，保留Shader1024迭代乘积安全门。


## 2026-10-19 相机镜头高光效果

Renderer::setLensEffects复制sanitizeLensEffects后的设置，默认关闭，非法浮点归零/合理默认并夹限。
主视图PostProcess在曝光/tonemap之前从本帧BloomBlurB读取HDR高光，产生1..4轴双向星芒、
三组镜面中心鬼影、RGB色散与可选静态程序化脏镜纹理调制。星芒按视口高宽比修正方向，
每轴16次有界采样，鬼影最多9次采样；这是一种屏幕空间艺术模型，不是衍射/镜片光线追踪。
设置开启且有效强度>0会独立启用既有半分辨率高光提取/模糊，即使Bloom贡献为0也有效。
关闭不增加RT或draw，shader跳过镜头采样。输入必须是当前帧已产出的高光；资源缺失时强度归零，
不读历史缓冲。灯口被不透明几何完整遮挡时不能生成鬼影；镜头效果可扩展到图像其他物体上。
曝光仍共享现有固定倍率/自动测光历史，lens不是第二套自动曝光。此阶段无实体镜片/灯壳几何。


## 2026-10-19 灯体与遮光几何

FixtureHousings::append显式追加不透明灯壳、镜片边框、筒形遮光罩与可选四片铰接遮光叶。
bodyLength[.5,10]、hoodLength[0,4]为出光口半径倍数，leafAngle[-.5,.5]rad为铰接角。
单位网格/材质在Renderer中共享，变更设置只改矩阵；不销毁保留场景仍引用的旧网格。
最多256灯/1536部件，灯灭保留实体，Point/零出光口跳过；非法输入保留准备输出/统计。
普通opaque深度、GBuffer与刚体批处理生效，背壳/罩/叶片可遮挡相机所见灯口。
灯壳金属为共享PBR材质，接受既有灯光/阴影；半径与方向跟随演出帧，外壳不随iris收缩。
这些是可见几何，不会重写光度/IES/Gobo或把叶片角自动转换为光束通光率；
光传播遮挡仍服从现有阴影预算/近裁剪精度，近灯口的机械挡光并非完整镜片追迹。


## 2026-10-19 有限出光口半影（PCSS）

PerformanceLight.areaShadow为可选模式，仍需castShadow与atlas槽，默认关闭保留旧硬影/2x2 PCF。
瞳孔半径=apertureRadius×iris；先搜索更近遮挡深度，线性化ShadowCaster存储的
.5+.5*(clipZ/clipW)，再估算世界半影R×(zReceiver-zBlocker)/zBlocker并过滤可见性。
SpotShadowProjection.h是阴影生产矩阵与采样侧near/far/FOV的一致入口；源投影按有限瞳孔/棱镜扩大，
覆盖近处光束口径，最大仍遵循175°FOV。接触处几何半影为零，远处/大瞳孔更柔。
areaShadowBias[0,.1]为独立世界单位偏移（默认.003），旧light.shadowBias仍只用于legacy NDC比较。
表面16、体积8个确定性分层样本，额外中心遮挡采样；搜索和过滤半径最多32个atlas像素，
夹回本灯子图，既有shadowSoftness作为过滤下限。0半径回退旧路径，采样不使用历史或随机帧噪声。
该PCSS是平行平面/平均遮挡深度近似，稀疏采样、atlas分辨率、近裁剪及半径预算会限制质量，
并非多光源面积蒙卡阴影。不会增添新的shadow atlas槽或改变旧八灯Shader ABI。
算法参考：https://download.nvidia.com/developer/SDK/Individual_Samples/MEDIA/docPix/docs/PCSS.pdf


## 2026-10-19 扩展灯透明 PBR 与独立 SSR

主视图 Deferred 透明阶段按原有后到前顺序提交 base，再追加扩展灯直接光。
复用不透明光学核及阴影绑定，包含光度/IES/Gobo/iris/焦面/棱镜/PCSS；仅标准 PBR ABI，
自定义 Unlit/粒子不强行改写。临时材质副本用独立 aypt_ 属性名，避免 bgfx 全局 uniform
默认值污染共享 PBR；源材质不变。RGB One/One、alpha Zero/One，直接光按源 alpha 加权，
非预乘 Additive 保持既有全量语义，深度只读；后层光仍被前层 alpha 遮挡。
PerformanceLightBudget.transparentDraws 默认128、范围0..1024，transparentLightsPerObject
默认8、范围0..32。来源还受surfaceLights约束，保留sortKey显式分层，同层按近到远分配额度，再按后到前合成；
刚性网格用保守AABB求交，无可靠包围盒/变形失败开放。统计区分提交、预算跳过和不支持材质。
0额度不建透明光程序；预算只是受控近似，不保证透明叠层的完整物理传播/折射/彩色透射阴影。

SSR现在可在没有IBL环境立方贴图、环境强度0的舞台工作；延迟建立黑立方和64x64 split-sum
积分LUT，关闭SSR不建立资源。已有IBL时复用其资源，命中替换原IBL镜面；反射的本帧屏幕辐射
已含照明，不再乘环境强度。输入包含不透明扩展灯直接光；保持既有8..64步与失败/未命中回退。
SSR位于体积和透明之前，仅反射可见不透明接收面，不反射透明层、后加发光灯口、离屏对象，
不提供折射、递归反射或全局间接光。生命周期销毁fallback纹理，资源失败不发布结果。


## 2026-10-19 单次屏幕漫反射

ScreenSpaceIndirectSettings 经 Renderer::setScreenSpaceIndirectSettings 复制/有限化，默认关闭。
主Deferred以独立开关复用SSR Pass/瞬态HDR目标，specular SSR可保持关闭；全关闭不增加RT/draw。
本帧LightingColor（含不透明扩展光）为唯一辐射输入，禁止历史/递归反馈。
每个StandardLit非金属接收面最多4条确定性余弦半球射线、每条4..32步、命中4次细化；
核对几何覆盖、厚度、自命中、朝向和屏幕边界，缺失样本贡献0。距离/厚度均为世界单位。
余弦抽样抵消Lambert的1/pi，漫反射权重为clamp(albedo,0,1)×(1-metallic)×.96，
再乘strength[0,1]并平均射线，保留alpha；金属1、黑反射率和零输入不增加漫反射。
关闭项不依赖环境贴图。固定方向避免逐帧随机闪烁，但1..4射线会有离散欠采样；
它不是离屏GI、探针、多次反弹、透明折射或时域累积。共享屏幕命中规则与SSR维护一致。


## 2026-10-19 低分辨率体积积分与深度重建

VolumetricSettings.downsample为每轴除数1/2/4，默认1保持原路径。2/4新增一个FrameGraph瞬态
RGBA16F积分目标，尺寸ceil(viewport/divisor)，存储散射RGB/透射alpha；扩展灯仍一次介质消光、
独立预算下逐灯积分和scissor，保留全部IES/Gobo/焦面/棱镜/PCSS。传统单灯也支持同一重建。
本帧积分完成后用专用视图242重建到既有全分辨率VolumetricColor，再透明/Bloom；
FrameGraph资源ID30追加VolumetricIntegration，旧ID不变，Count31。关闭或除数1不分配此附件。

重建仅采散射，四点双线性权重结合GBuffer视空间深度和天空/几何分类；深度容差max(.05,.02×z)，
4倍容差外拒绝，未覆盖的细前景不借背景散射（零散射回退）。消光仍在全分辨率按实际观察路径
计算，动画密度沿用4点积分，场景RGB与alpha始终来自全分辨率源；不会把背景一起缩小模糊。
增加一个重建draw，volumeDraws仍只数光源。低采样仍可能漏掉很细的光束/前景，不是froxel或历史重投影。

242纳入固定视图保留，离屏UI分配器自动跳过；现有各ID与camera64..111不变。
UIRenderBackend.kMaxOffscreenPaintsPerFrame从145变144，与实际保留容量一致，四视图组仍可留下空隙。


## 2026-10-19 合批与逐视图性能诊断

FixtureHousings::append按Body/Hood/Leaf共享网格分组，保持各部件矩阵与组内顺序，
让既有GBuffer/Shadow刚体实例合批连续生效；buildFixtureHousingDraws的离线输出顺序不变。
16灯/96部件D3D11和D3D12均从102后端draw降到9，instancedObjects96，RGB回读差0。
实例对象数含多Pass/阴影重复，不等于独立实体数量。

setDebugOverlayEnabled(true)现在同时启用BGFX_DEBUG_PROFILER；默认关闭查询。
setDebugOverlaySuppressed(true)隐藏文字但保留采样，getFrameStats为逐帧更新的借用快照。
DebugOverlay归并补全SSR32、体积33/242、Bloom11/12/26..30及曝光31；
独立相机/动态离屏UI不逐项归属，0分项表示未启用/不支持，不应解释为零开销。
GPU完成帧可滞后CPU；Renderer CPU帧内时间不覆盖Host的演出求值/Presenter工作。


2026-10-19 局部雾重建补充：重建消光的4点密度规则同时应用局部盒.75..1柔边权重与动画噪声，
修复局部设置下散射和消光密度不一致。常量全局路径保持原解析式。
真实GPU回归新增局部/全局×静态/动画×1/2/4分辨率对照，两后端各1项1298断言通过。


## 2026-10-19 有界 froxel 系数网格

VolumetricSettings.froxel默认关闭；tile4/8/16、深度16/32/64，cell预算65536..4194304、默认1048576。
XY分辨率同时受纹理轴限与cell预算约束。RGBA32F八列atlas保存RGB入射散射系数和alpha消光系数，
局部柔边/动画密度独立写一次，各灯复用完整光学与阴影核，只加RGB，随后按观察距离对数切片做Beer积分。
默认1920x828、tile8、32切片为240x104x32=798720cell（field约12.2MiB），不分配逐格灯列表。
无RGBA32F framebuffer能力回退原ray路径；网格启用时downsample不参与布局。极细图案可能欠采样。
场景颜色/alpha仍全分辨率，深度感知重建及当前介质消光沿用既有路径；无历史或多重散射。
新增FroxelField资源ID31，原ID不变；view241注入、33积分、242重建，UI单Pass容量143。
RenderFrameStats.volumetric报告实际积分宽高、切片、cell数及启用状态；关闭帧清零。


## 2026-10-20 独立体积系数历史

froxelHistory默认关闭，weight=.85[0,.95]；仅网格路径启用。两份RGBA32F最近采样历史由Pass持有、
FrameGraph按32/33导入（额外32bytes/cell），view240位于241注入和33积分之间；UI容量142。
前帧系数按世界位置重投影，并由当前5点邻域夹紧；零通道/亮度或消光突变拒绝历史，当前alpha不滤波。
尺寸、网格/物理参数、镜头大切换、提交帧间断或noiseTime跳变失效；显式resetVolumetricHistory处理跳播/换场。
失败退回当前场，关闭释放缓存；成功完成全分辨率输出后才交换历史，不依赖TAA/曝光结果。
historyUsed是帧级重投影资格，不是逐格接受数；最近采样、无子格抖动/方差估计，仍可能漏掉细光束。

FrameGraph::resolve对external借用也要求本帧declared：效果关闭未重导入时返回invalid，
避免Owner已释放旧FBO后仍查询其附件；本阶段历史关停真实GPU用例覆盖此路径。


## 2026-10-20 点实例连续写入

实例数据按批次一次resize后写完整64字节记录，避免每点vector::insert与迭代器扩容检查。
沿用相同裁剪、稳定ID预算、半径/覆盖能量及16浮点布局；缩放重用和16383边界有CPU回归。
独立D3D11/D3D12点实例与CPU四边形读回对照通过AY_POINT_GPU_TEST选择后端。
这项CPU准备优化不等同GPU筛选/compaction或点光聚合稳定性。


## 2026-10-20 局部密度场

VolumetricDensityField::create复制各轴1..64的uint8数据，CPU三线性采样；createVolumetricDensityTexture生成
8列线性RGBA8 atlas。setVolumetricDensityField验证配对与活句柄，active时强制local=true，普通数值设置不丢绑定；
destroyTexture/shutdown清除绑定并失效独立体积历史，host纹理需活过渲染。场在盒外0，叠加原.75..1柔边和动画噪声。
ray、froxel密度写入、全分辨率消光与CPU灯口读取同一场；GPU两层Z插值并使用显式LOD0避免动态循环导数展开。
分离消光/灯口最多16采样，legacy单Pass可共用最多64 ray步，froxel每格中心；高频网格仍可能欠采样，非精确体素积分或流体。
网格图案是消光倍数，不新增彩色透射、透明雾积分、多重散射或硬件控制。

### 连续点阵聚合与有界排名过渡

EmissivePointLightingSettings.smoothCells默认false；true把RGB按三线性权重分给8个固定世界网格节点。
预算/钳制前保持RGB和位置一阶矩，±100000世界边界节点截断除外；不再跨硬桶突变，不能保证预算裁剪后能量守恒。
范围扩展sqrt(3)*cellSize并受10000总上限约束；最多524288候选节点，最多64灯，仍独立于可见点数预算。
EmissivePointLightingHistory::update接收time和transitionSeconds[0,1]，只用于smoothCells。
最多maxLights个槽，旧节点先淡出释放，再准入新节点淡入；可能出现短暂能量降低，保持原RGB不改变色相。
同时间不推进，逆时/超过.25秒缺帧/配置变化重置；全黑立即清空。显式seek/编辑调用reset；默认无历史。
Presenter setPointLighting(settings,transitionSeconds=0)保留原路径，可显式resetPointLightingHistory；Studio跳播会重置。
Studio --point-smooth、--point-transition秒，点照明按钮按off/hard/smooth循环；不改变schema8。

Point PerformanceLight可选pointContactDistance[0,10]、pointContactThickness[.001,5]、pointContactBias[0,.5]、pointContactStrength[0,1]。
距离0默认关闭；EmissivePointLightingSettings的contact*复制到聚合灯。共享不透明/标准透明受光绑定执行最多8次采样。
使用当前GBuffer世界位置/覆盖率沿向光方向短距追踪，比较相机空间前向深度以判断可见遮挡；厚度和偏移是世界单位。
只检测当前视图中的不透明遮挡，不能处理离屏、背面、多层透明或完整点光阴影；不占cube/Spot shadow槽。
Studio --point-occlusion距离显式开启。现有Spot必须保持pointContactDistance=0。

连续聚合缓存每个硬格的8邻节点索引（最多65536个邻域），同格点不重复8次哈希；零权重角按需创建，无空能量节点。固定节点不计算未使用的重心。

### GPU点阵筛选、压缩与间接绘制（显式会话选项）

EmissivePointSettings.preferGpuCulling默认false；Windows D3D11/12具有compute/instancing/indirect及RGBA32F image-write时可用。
仅当source.size()<=maxPoints才走GPU；较小显示预算整批回CPU稳定ID排名，preferInstancing=false也保留CPU四边形。
源数据仍在CPU求值/验证，32-byte输入上传；mark、256点分块计数、前缀和、按原顺序压缩4vec4/64-byte实例，最后一个索引间接四边形绘制。
GPU与CPU共享XY/w裁剪、最小像素半径及面积能量公式，不额外加Z裁剪；可见筛选不影响独立的CPU点照明聚合。
Renderer每设备最多8个存活GPU cloud state；缓冲按256..65536的二次幂扩容，不占瞬态实例预算。
4个compute及raw draw vertex/fragment共6阶段在构建时烘焙为s_5_0 DXBC并嵌入，无运行时shaderc路径；78个标准生产变体仍单独覆盖。

cloud.stats()是append刷新的值快照。GPU模式countsExact=false，visible/culled为未知0，不能称为实际0可见；source仍是当前输入数。
gpuDiagnostics默认false，正常GPU绘制不读回、不等待。开启后，gpuSampleValid/source/visible/serial标记较早提交的异步样本，可能跨多个帧。
计算、blit、readTexture请求分开提交，越过返回完成帧后才消费；一个state最多一个在途读回。销毁state会取消尚未排队的请求，
已排队CPU目标由Renderer context保活到完成/设备关闭后，避免销毁cloud时写入已释放内存。
GPU流在OwnedCpu RenderScene中仍借用Cloud状态，必须保持owner到render结束，不能跨下一次append保留GPU帧；设备重建后重建Cloud/Presenter。

固定view55/56/57/58为mark/count/prefix/compact，59为诊断blit，排在主渲染之前；与云和艺术后处理合并后UI离屏单视图上限为132。
RenderFrameStats.pointGpuTimeMs/pointGpuReadbackTimeMs分别统计四个计算视图和blit，GPU帧号/0含义沿用原诊断；后者不含CPU映射或排队延迟。
Studio --point-gpu显式启用，--point-gpu-readback再启用带序号样本；--point-cpu保留CPU四边形。JSON增加gpuPointCulling、pointCountsExact、
gpuPointFallback、gpuPointSampleValid/Serial/Source/Visible及meanPointGpuMs/meanPointGpuReadbackMs。
CameraRenderView即使没有不透明几何也执行请求的颜色/深度清屏，避免纯点阵或空帧保留上一张输出。

### 体积云分层密度组合（2026-10-24）

CloudSettings.structure的blend默认0，保留原密度与光照；非零时将独立低频主形与中尺度圆团/折叠梯度组合，
cellular、billow为[0,1]相对权重，warp为周期扭曲，detailStrength只侵蚀已有边缘。
混合主atlas为linear RGBA：R主形、Gcellular、Bbillow、A独立变化；现有2D切片图集尺寸与借用生命周期不变。
较大尺度用world/(scale*2.8)+phase独立寻址，避免非整数频率放大归一相位的周期跳变。
不同CloudForm控制云顶/云底和层状连接，层积/雨层的底部也由中尺度结构调制，不只改变云顶。
新路径保留太阳密度自遮蔽，采用三个衰减阶的补光近似；仍无完整多次散射、大气求解、地面云影或云专用重建。
引擎环境静态示例对照四套来源/生成方式不同的噪声包和四组算法权重；外部原件先入AssetRepo。
AYRenderer_CloudRenderTest可额外接第四参数lab目录，强制加载四包并检查密度差异、算法差异、扭曲、周期与光照。
运行命令及视觉证据见引擎AYDocs/ENVIRONMENT-DISPLAY.md；CPU/GPU断言不替代静态美术审查。


## 2026-10-24 — 乌云光照与远景衔接

CloudSettings.light默认沿用场景首个方向光；overrideScene=true可提供遮挡之前的线性radiance与光线传播direction，
由同一密度执行云内自遮蔽，不再把地面阴天衰减误用到云顶。不会修改SceneLights或地表受光。
方向归一化，退化回落向下；radiance有限并限制0..10000。无需新增uniform或FrameGraph资源。
Stratus混合密度的底部由多场浅起伏组成，减弱重复圆碗轮廓；积云/层积形态公式保持。
CPU覆盖光源边界；实机CloudRenderTest比较地面暗光、独立入射、零入射、关闭覆盖四态。
