# AYRenderer 测试维护

## 静态形态族与薄云片（2026-10-24）

CloudForm新增五种显式形态并保留旧Blended；增加独立高空解析薄云片、可选samplingPhase积分偏移。
最终Windows Debug定向CTest六项通过（14.27s），含环境字段、云通道、粒子显示、Renderer桥、CLI及Contracts。
独立无Renderer/Weather/JSON构建5/5通过（2.50s）。生产ShaderBuild实际编译78变体，最终增量复用校验通过。

D3D11/D3D12完整云GPU序列均通过；保留旧空场/前景/自遮蔽/phase/resize等检查，
新增同覆盖率显式形态差异、薄云与低层云差异、高云独立提交/前景深度，以及积分采样相位改变/周期绕回。
旧Blended天空差110.001、前景差0，验证既有默认路径没有被新形态开关替代。
日志与像素：E:/CodexBuilds/static-cloud-aa-final-d3d11、static-cloud-aa-final-d3d12。

静态宿主冻结天气、世界相位与粒子，通用TAA加黄金比积分采样，96帧后采图；案例切换notifyCameraCut。
七种960×540 D3D11图已逐张查看，另检查转90度、仰视、高处与seed17，以及D3D12积云。
同案例两后端平均RGB绝对差0.102236；云体ROI高频残差0.53648→0.19359，仅作该图噪声诊断，
不作为云型美术达标或性能证据。实际交付在out/environment-static-sky。
薄云仍偏平、高处观察与高耸云细节仍需细化；高空片层未默认叠加。未验收短时动态或跨天气过渡。

## 连续云与空间天气场（2026-10-24）

云密度改为连续三维场与独立层云/积云高度剖面，新增云型、聚集、向内软边缘、世界空间
RGBA天气场、局部雨幕、天空渐变和距离空气透视。Windows Debug生产ShaderBuild生成78变体。
最终Contracts、环境字段/通道/粒子显示/桥五项CTest通过（6.21s），独立CLI Cook测试通过（4.45s）。

独立真实GPU序列D3D11和D3D12均通过，skyDelta=110.001、foregroundDelta=0。
同coverage改变云型确实改变像素；软边近透明区从2430增到15384像素；验证RGBA独立通道、
场外清空、repeat、仅降水场与干场、关云后的天空渐变以及前景不被天空阶段重涂。
现有自遮蔽、phase绕回、真实128³/32³ atlas与关闭/resize检查继续通过。
云团从下方/上方测得晴空63.1537%/48.2463%、厚云20.89%/39.0062%、云顶变化89/25px。

本轮修正测试fixture：resize后显式更新viewport到400×200，且逐像素检查alpha=255，
避免旧320×180 viewport外黑边被错误计为厚云。下方历史形态统计含旧fixture，不能与本轮直接比较。
没有修改Renderer resize与viewport各自独立的API契约。

日志/像素在E:/CodexBuilds/weather-gpu-fourth-d3d11、weather-gpu-fourth-d3d12；
环境七种960×540最终画面在weather-gallery-final，人工核对四类天空与有/无雾对照。
本机交付入口out/environment-weather-gallery/weather-gallery.cmd；不声称物理天空、时域降噪或性能等级验收。

## main合并门禁（2026-10-23）

合并灯光演出与main的云/艺术后处理、资源观察和设备清理实现。固定view避免冲突：
既有50..54保持，点计算/读回55..59，体积240..242，完整顺序75项；UI容量132。
FrameGraph旧0..36不变，追加37..40，Count41；原有数值/顺序断言同步保留精确检查，
Deferred的Clouds占位同步到28项。标准包78变体，GPU点计算另有6个构建时嵌入阶段。

本机Debug定向实际通过：Contracts216/3566、SceneMaterial108/12390、LightingShadow138/1517、
PostProcess269/1797、FrameGraph41/159、UI120/461、Shader12/220（用例/断言），合计904项，
均无跳过。D3D11与D3D12分别运行EmissivePointsGpu2项231断言和FroxelVolumeGpu1项46断言，
全部通过，无跳过；这不是完整GPU矩阵。NavigationPlayground的core/worksite/host/render四项
也实际通过，含3次交付、6份维修、抢占恢复与真实GPU截图落盘。
主仓SDK doctor149能力、57模块、127检索、707入口，0错误/0警告。
日志与JSON在C:/Users/lenovo/.codex/tmp/main-merge/validation。未执行Release、跨平台、
完整Renderer全部分区、人工桌面或新的性能基准，不把分支原有验收重写为本次结果。

## 云团结构与外部atlas（2026-10-23）

本轮Contracts/环境桥2/2通过（1.85s，Contracts171例，桥24检查），覆盖3种atlas布局、
独立天气场的seed/分布和公里级相对投影；ShaderBuild实际生成71个Windows/s_5_0变体。
AYRenderer_CloudRenderTest新增可选第三参（导入云PNG所在目录），真实使用128³ base与32³ detail。
D3D11/D3D12完整通过：skyDelta=110.005、foregroundDelta=0，含phase 0/1绕回、远离原点相机，
以及从下方/上方读回的云团透射。clear比例0.610038/0.630563，thick比例0.365775/0.337213，
云顶像素范围变化180；断言晴空>15%、厚云>1%、云顶变化>10px，排除均匀云盖假通过。
原始图像/日志：E:/CodexBuilds/cloud-gpu-final-d3d11、cloud-gpu-final-d3d12；
环境桥七视图人工检查位于cloud-gallery-final，交付副本out/environment-cloud-shape。
side视图仍有有限步数噪点，验收不等同于电影级天空或时域/性能认证。
外部素材离线转换的3项格式测试在主仓tools/assets，来源/许可证见AYDocs/ASSET-SOURCE-WORKFLOW.md。

## 单层体积云（2026-10-23）

MSVC19.51/Windows，E:/CodexBuilds/AliyatEnvironmentDisplay-20261023：
Contracts/SceneMaterial/UI及环境桥4/4通过（63.30s）；补充投影重建和最终改动后Contracts/环境桥
定向2/2通过（1.88s），Contracts170例2014断言，环境桥24检查。CPU证据为独立设置/FG/顺序、
周期噪声边框/种子、相机点重投影，以及无效资源resolver前置拒绝和设置恢复。

独立真实设备目标AYRenderer_CloudRenderTest：D3D11和D3D12均通过完整像素序列；
最终skyDelta均104.808、foregroundDelta=0（8-bit RGB平均差）。验证实际云对天空的吸收、
前景遮挡、外部零密度/零天气图、光照步进自遮蔽、程序噪声phase变化、禁用无旧帧和400×200 resize。
命令：`AYRenderer_CloudRenderTest <fresh-directory> d3d11`（或d3d12）。
每帧还核对Clouds阶段提交，不以有截图文件代替云生效。源、阶段、设备和输出目录均为显式输入。

实际ShaderBuild生成71个Windows/s_5_0内置变体（含云），production目标校验复用成功。
环境示例D3D11以--clouds采集七种960×540画面，逐张核对代表晴/雨/夜与移动/关闭对照。
原始像素与日志在E:/CodexBuilds/cloud-pixels-d3d{11,12}-accepted、cloud-captures-accepted；
主仓out/environment-cloud-render保存日志、JUnit、截图与启动入口。
SDK doctor最终142能力/57模块/110搜索/649文档符号，0错误/0警告。
初次项目逆矩阵产生常量错误射线、最初像素断言失败；云阶段改用double消元、残差校验和
齐次角点插值后两后端完整测试通过，未改变AYMath或其余效果的逆矩阵契约。
初版没有地面云影、历史重投影或性能等级认证，不外推到Forward/独立相机视图。

## 固定View GPU计时归属（2026-10-22）

`timing-ack-before-timing.json`在原Renderer库下1例25断言中6失败，非零fixture每View独立2ms：
Bloom期望14ms、曝光/SSR/体积各2ms、16个Decal合计32ms、透明与三种选择mask合计8ms。
CPU时间/draw仍保留；动态相机/UI离屏与GPU粒子不归入这些主Pass，不存在的CPU行不新增。
`timing-ack-baseline-valid-proof.json`核对测试加入前后两个实现库hash均未变。
测试归属SceneMaterial分区；修复后同1例25断言通过，`timing-ack-first-timing.json`。
完整消费方172步构建通过，最终冻结回归`render-final-*`全部结束：
九组CPU1020例18581断言、Editor主入口471例12538断言及独立装配1例13断言、
Entity提交专项9例153断言通过；两后端完整Gpu各58例45072断言、Shader原生移动各42断言通过，
无失败/跳过。两后端Entity静态/蒙皮及Workshop/AssetReimport/MaterialPreview六项实际流程通过。
总日志`render-final-regression-run.log`确认冻结的五模块CPP/H摘要未变，
`render-final-frozen-source-hashes.json`固定来源；清单Renderer1078/10、Editor472/2、Shader498/3无遗漏重复。
最终SDK128能力/53模块/102搜索例/576文档入口，零错零警告，见`sdk-render-final.json`。
这些是当前版本的最终门禁，不与下文历史阶段重复累计；纯计时fixture仍不能证明设备查询的帧来源已改变。

## 同池UI程序与部分初始化（2026-10-22）

旧Renderer `7247994`只加入两个真实GPU探针并重链测试，库hash未变，
`ui-pool-baseline-valid-proof.json`记录摘要；两后端所有基线进程均正常完成。
`ui-pool-before-pixels-*`各1例58断言中9失败：第二backend关闭/重开关闭/重建后关闭的
六条flat/SDF像素检查失败，每个内域3456个通道差异；三条原生program计数失败，CPU draw计数仍为2。
`ui-pool-before-partial-*`各1例15断言中3失败：white索引未清空、已借用flat token失效、program计数减少。
探针在旧white索引残留时安全提前返回，未故意执行重复destroy，不把潜在双释放写成已获崩溃堆栈。

新实现`ui-pool-first-pixels-{d3d11,d3d12}.json`各58断言全过，七阶段的两类内域与独立颜色参考无差异，
包含反向关闭、同对象重新初始化，program复用与临时纹理回收稳定。
`ui-pool-first-partial-*`各48断言全过：缺SDF包失败、三次失败重试、重复shutdown，
补装SDF包后同context初始化成功，关闭UI保留池缓存，最后pool shutdown后原生program/uniform/texture计数归设备基线。
两个backend按同一owner顺序使用，没有验收并发绘制；自有white和借用程序分别管理。
真实修复图在`ui-pool-first-native/`，已检查D3D11关闭第二backend后的flat/SDF仍可见。
完整消费方门禁另行记录，不能混入前一阶段56例的冻结结果。

完整消费方42步构建通过；`ui-pool-cpu-final/`九组1019例18556断言、
Editor470例12510断言、Entity9例153断言通过。两后端完整Gpu各58例45072断言，
Shader原生移动各42断言，无失败/跳过；两后端Entity静态/蒙皮及六项实际Editor流程通过。
`ui-pool-regression-final-run.log`完整结束并复核冻结源未变，记录`ui-pool-frozen-source-hashes.json`。
清单1077项/十分区无遗漏重复；SDK128/53/102/576零错零警告，最终根文档仍需统一校验。
该批次未包含后续GPU计时归属或混合确认提示的新探针。

## 设备保持存活时的管线清理（2026-10-22）

`renderer-keeper-first-valid-{d3d11,d3d12}.json`两后端各1例36断言通过。
独立Adapter保持真实设备运行，Renderer执行Deferred/SSAO/PostProcess→Forward→Deferred，
原生VB/IB在切换后回到资产基线，程序缓存仍归同池；关闭后纹理/VB/IB/FBO/program均回到设备基线。
随后同一Renderer实例重新初始化，不额外configure掩盖旧Pass句柄，实际SSAO/PostProcess再次提交，
上下文id更新，第二次shutdown后五项计数仍回到基线。这不是设备shutdown强制回收后的观察。
keeper只维持同一设备，不同时绘制第二套主管线，不能据此宣称多Renderer并发提交/多窗口切换已验收。
本用例在`7e75f7b`之后补充；不能混入此前55例的冻结统计，当前完整门禁另行记录。

Shader `3d5412c`修复池移动赋值后，完整消费方构建通过；
`shader-move-cpu-final/`九组1019例18556断言、Editor470例12510断言、Entity9例153断言通过。
两后端完整Gpu各56例44966断言、Shader原生移动各1例42断言，无失败/跳过；
两后端Entity实际静态/蒙皮及Workshop/AssetReimport/MaterialPreview六项流程通过。
总日志`shader-move-regression-final-run.log`完整结束并复核冻结源码未变；
清单1075项/十分区无遗漏重复，SDK128/53/102/576零错零警告。
本批冻结文件为`shader-move-frozen-source-hashes.json`；后续UI共享池探针尚未加入本阶段二进制。

## 管线清理/reset边界（2026-10-22）

`reset-lifecycle-before-{d3d11,d3d12}.json`：两后端各1例306断言，4项失败。
同一Renderer以关闭Decal作对照，reset后只推进设备帧、不render；resize纹理/FBO期望3/0、实际12/2，
MSAA期望5/2、实际14/4。其余302断言通过，包括原先的实际像素、culling/parity和再渲染检查。
编译只增加GPU测试断言，Renderer库hash前后相同；`reset-lifecycle-baseline-proof.json`记录旧库与新测试摘要，
基线实现为`6c211f5`。这是reset当场回收边界的复现，不是永久泄漏或驱动崩溃证明。

新实现`reset-lifecycle-first-{d3d11,d3d12}.json`各1例306断言全过：两种reset的实际计数均等于对照，
随后重新render仍正确。首批完整Contracts167例1966断言通过；之后补充disabled producer断言，需以最终门禁为准。
29种工厂类型/26种GPU cleanup的源码归属已机械核对，无遗漏或额外类型；这不替代实机生命周期证据。
完整消费方构建（含EditorShell/MaterialDemo/SampleTool）通过。
最终九组CPU1019例18556断言、Editor470例12510断言、Entity9例153断言通过；
两后端完整Gpu各55例44930断言，无失败/跳过；两后端Entity静态/蒙皮及六项实际Editor流程通过。
报告为`reset-lifecycle-wired-*-final.*`，总日志完整结束，库存1074项/十分区无遗漏重复；
SDK128/53/102/576零错零警告。

首批全量在PostProcess两项旧内联位置断言失败，随后在Audit另有一项同类断言失败；
它们改为检查清理边界先于clear/shader pool/adapter，运行期资源断言不变。
最后只重建Audit测试TU：其余八个已通过分区的源码和执行文件经hash逐项核对未变，
复用原报告并新跑Audit，`reset-lifecycle-wired-reused-cpu-proofs.json`、`reset-lifecycle-pre-audit-runtime-hashes.json`
和两次冻结源摘要记录来源。没有用旧实现的GPU结果验证新helper，所有最终GPU/Editor流程均重新执行。

## Decal附件接纳（2026-10-22）

`decal-admission-first-cpu.json`：DeferredDecal合约8例89断言通过。
新增用例在未初始化/Noop下检查整数边界、格式、空/无效附件和统计不变。
两后端`decal-admission-first-{d3d11,d3d12}.json`各1例175断言通过：
实际五附件借用壳清除/采样，销毁壳不释放颜色/深度，再建壳后第二颜色读回正确；
空/重复/无效/256附件请求不分配，Decal分配八颜色/两壳、重复prepare不分配，
无效尺寸保留两个输出与计数，最终先销毁Pass再销毁base GBuffer回到初始计数。
这些测试没有向旧实现发出危险的大尺寸设备分配，不声称完成旧驱动故障复现。
四项既有实际Decal测试在两后端分别通过：ping-pong/resize/借用depth531断言、
culling/parity/整批校验387断言、主Renderer graph/final pixels216断言、材质/normal1390断言。
本阶段冻结完整九组CPU1019例18580断言、Editor470例12510断言、Entity9例153断言通过；
两后端完整Gpu各55例44840断言，无失败/跳过；`decal-admission-*-final.*`。
两后端Entity实际静态/蒙皮与Workshop/AssetReimport/MaterialPreview六项Editor流程通过，
`decal-admission-regression-final-run.log`完整结束。库存1074项/十分区无遗漏重复，SDK128/53/102/576零错零警告。
`decal-admission-frozen-source-hashes.json`固定本阶段；Test_PhoskiaGpu的冻结副本另存，
后补reset后不render的计数断言不在这批二进制或提交中，不能据此外推其已通过。

## UI段落的独立参考（2026-10-22）

先用新测试独立编译的TU链接冻结旧Renderer库：19例中3失败、108断言中21失败，
涉及hard break/空行高度与宽度、lineSpacing、控制字符不应进入glyph批次；`ui-paragraph-before.json`。
构建输出全部在证据目录，主构建的对象/库/程序没有被覆盖；旧UI源码副本与冻结源hash相同。
`ui-paragraph-baseline-proof.json`记录旧库、旧源码、新测试摘要，启动/构建失败不算复现。

修复后`ui-paragraph-first.json`19例108断言通过，无跳过。
两后端`ui-paragraph-gpu-first-*`各1例21断言通过：实际字形对独立七行参考，
1/1.25/2缩放，Left/Center/Right、Top/Middle/Bottom、空段/CRLF/CR/Unicode分隔、中英文及阿拉伯文，
附加outline/offset shadow和父裁剪；每组两半RGBA±1差异像素均为0，参考图有实际可见像素。
`ui-paragraph-native-first`保留真实PNG，不能用相同段落实现再次绘制当唯一oracle。
新完整冻结回归九组CPU1018例18562断言、Editor470例12510断言、Entity9例153断言通过。
两后端完整Gpu各54例44665断言，无失败/跳过；`ui-paragraph-*-final.*`。
两后端实际Entity静态/蒙皮，以及Workshop/AssetReimport/MaterialPreview六项Editor流程通过，
实际资源面板截图已逐图检查，LF控制字形已消除。库存1072项/十分区无遗漏/重复。
SDK128能力/53模块/102搜索例/576文档入口，0错误0警告。
冻结源码见`ui-paragraph-frozen-source-hashes.json`；后补Decal附件接纳与本节冻结门禁和文本提交分开，
不能从本节结果外推；其后验证见上节。

## 资源驻留观察冻结门禁（2026-10-22）

纯逻辑存储2例101断言与RRM/Noop6例1147断言通过：矩形/奇数窄轴/块/mip/六面参考，
未知borrowed、raw R8、dynamic初始零上传和非累计更新、展开/前缀链、采样默认值、
515项完整总计/512行上限/UTF-8 key边界、拥有值在销毁/关闭/重建context后仍可读。
原8例1242断言后拆纯计算两例，新增context检查使现合计8例1248断言；不是重复新增两批用例。

`AYRenderer_ResidencySafetyAsan`临时独立target编译纯存储计算、现有TextureUploadPlan/解码器与其测试，
15例541断言通过，无失败/跳过；`residency-safety-asan.json`。
不包含RRM GPU表/Editor或整个引擎，第三方bimg未重新插桩。
临时CMAKE_PROJECT_INCLUDE在finally中清除并重新配置，`restore-residency-safety-profile.log`保留恢复证据。

冻结资源观察版本完整九组CPU1014例18512断言、Editor469例12495断言、Entity定向9例153断言通过。
两后端完整Gpu各53例44644断言通过，无失败/跳过；`residency-gpu-final-*`。
两后端Entity实际静态/蒙皮及Editor Workshop/MaterialPreview四项关联流程通过，
`residency-regression-final-run.log`完整结束。Noop/CPU不能替代这些实机结果。
SDK128能力/53模块/101搜索例/576文档入口零错零警告；最初新tag抢占texture mipmap查询的失败报告保留，
随后调整专用tag且恢复原查询首位，通过`sdk-residency-routed.json`。

实际`residency-floating-native-{d3d11,d3d12}`已通过：GPU拒绝时仍是旧GPU尺寸/层级/负载，
BC1三层8+8+8字节，cube六面96字节和CPU副本分别计量，销毁清除，Scene/预览保留及修复/保存重开。
截图已检查，浮动控件可见；仍有旧文本后端的LF控制字形问题。
`residency-frozen-source-hashes.json`固定本阶段源；后续UI段落修复源码已另行开始，
尚未重编，不混算为这些冻结程序已通过的行为。

## 作者刷新确认与注册寿命（2026-10-22）

`refresh-bridge-first.json`23例444断言通过；包含未驻留不加载、两种颜色变体独立结果、
拒绝保留/修复、Renderer移动及后注册observer保留。`refresh-gpu-first-{d3d11,d3d12}.json`
各1例1456断言，CPU发布16385×1成功但设备拒绝后旧各级可读，修复后稳定id下更新。

冻结Resource/Renderer实现完整CPU1006例17264断言、两后端完整Gpu各53例44644断言通过，
无失败/跳过；报告`refresh-cpu-final/`与`refresh-gpu-final-*`。库存1059项无遗漏/重复。
Entity完整220例2938断言、定向9例153断言和两后端static/skinned实际场景通过。
六项实际Editor Workshop/AssetReimport/MaterialPreview流程通过；
该Editor程序是来源隔离补充前的ABI45版本464例12353断言，不验证后补map/预览新断言。
后补Editor来源隔离及额外预览保持验收独立记录，不改变本轮已冻结Renderer结果。

完整D3D12 suite实测约341秒，先前阶段亦达到351秒；Gpu watchdog改为600秒。
`refresh-gpu-watchdog-verified.json`核验CTest注册值600；上述完整GPU结果来自直接运行测试程序，
没有冒充已运行CTest wrapper或把回归耗时当作性能基准。

## Mesh GPU版本与Entity提取（2026-10-22）

修改前安全复现：`mesh-index-before.json`一例3断言中2失败；
`entity-submission-skinned-before.json`五例53断言中18失败，涉及重导入范围/palette、destroy和Renderer重启。
当前定向资源桥`mesh-bridge-residency-cache.json`为21例380断言；
Entity冻结契约`entity-submission-freeze.json`为9例153断言，均无失败/跳过。
纯准备器及定向ASan各8例137断言通过；ASan只覆盖准备器/测试与所链Resource，不代表整引擎GPU生命周期插桩。

`entity-mesh-frozen-{d3d11,d3d12}`各含static/skinned两个实际设备场景：
对独立范围/slot/palette参考RGB±1比较；扩展3→6索引分别变化7242/9238像素；
L2-only、CPU新版但GPU桥拒绝、旧快照/画面保留、修复重试、显式mesh/material destroy与新Renderer实例均通过。
新实例先加载blue材质占用低数值id，验证旧context的同数值handle不能污染red/blue画面。
30次额外提取的设备纹理、VB/IB及program数稳定。临时几何为测试输入，没有新增引擎资产包。

72次串行Debug/Noop提取对照在`submission-benchmark-final/`，源/二进制摘要及无干扰记录齐备。
每组120帧、三独立进程；静态4096/128路径约93.04→15.78ms，蒙皮约59.37→59.41ms。
小组多路径的蒙皮仍有检查成本；冷加载、上传、编译、渲染及写盘不计。不能作Release/GPU FPS保证。
早期smoke/path-review只用于定位问题，不与最终样本混用。完整模块回归另按最终结果登记。

冻结阶段完整CPU九组1004例/17200断言、Entity完整220例/2938断言、定向Entity9例/153断言、
Editor463例/12308断言均通过。两后端完整Gpu各53例/44219断言通过，库存1057项无遗漏/重复。
报告为`metadata-cpu-final/`、`entity-{full-,}metadata-final.*`、`editor-metadata-final.*`及
`metadata-gpu-final-{d3d11,d3d12}.*`。这些程序基于Resource`0c56907`与Editor`91ffba6`构建，
不包含后续通用刷新报告/ABI45原型。D3D12回归期间存在后续Resource编译，耗时不能用于性能结论；
性能仍只引用先前无干扰的72组提取与独立串行画质记录。
`metadata-native-{MaterialWorkshop,AssetReimport,MaterialPreview}-{d3d11,d3d12}`六项实际Editor验收均通过，
覆盖Scene/预览隔离、重导入、Cancel/Apply、坏源保留/重试及保存重开。SDK最终127能力/53模块/
97搜索例/569文档入口，0错0警告；132个本地文档目标存在，锚点未由该检查器验证。

## Cube 与渲染目标创建接纳（2026-10-22）

先以已有1×1缓存key请求65536/UINT32_MAX cube，安全复现旧路径在尺寸检查前返回缓存项。
`cube-admission-before.json`为1例10断言、2个失败的原始证据；未执行可能读取短缓冲或分配巨量内存的旧路径。
修复后`admission-bridge-final.json`为19例338断言通过；定向真实设备
`admission-gpu-final-{d3d11,d3d12}.json`各1例1653断言通过，无跳过。

最终完整CPU九组994例/17021断言通过，无跳过；`admission-cpu/`。
全Gpu分区D3D11/12各53例/44219断言通过，无跳过；`admission-gpu-full-*.json`。
库存1047项、十分区无遗漏/重复，Editor463例/12308断言通过。
两后端`admission-native-{MaterialWorkshop,AssetReimport}-*`通过，包含未保存Scene变化0、
保存显式重载后变化、真实导入Apply/Cancel、失败保留/重试与Scene保存重开。
SDK127能力/53模块/97搜索例/556文档入口零错零警告。本阶段没有新增ASan覆盖。

检查包含非法尺寸、设备上限、256MiB六面预算、无缓存拒绝、非法格式、缓存及纹理/FBO数量保持。
正向检查包括raw六面独立颜色、可更新cube的六面×三级颜色、preferred-color回退及全部资源回收。
这证明创建入口的接纳与正常绘制，不等于覆盖任意外来更新矩形、设备丢失或GPU OOM。
`createColorDepthFrameBuffer`保持首选格式回退语义；严格格式入口仍拒绝无效枚举。

## 管线职责归位与设备门禁收敛（2026-10-22）

产品描述/纯兼容策略与具体Pass工厂从主Renderer移出，默认链、公开布局与shader键不变。
八个纯函数去除注释/空白后相同，29个槽/类型映射相同，证据为
`out/render-sampling-20261021/assembly-mechanical-review.json`。
九组CPU992例/16989断言通过；`assembly-cpu/`。库存1044项、十分区无遗漏/重复。

完整Gpu分区在D3D11、D3D12各52例/42566断言通过，无跳过；
`assembly-gpu-aligned-{d3d11,d3d12}.json`。现在统一使用`AY_TAA_GPU_TEST=d3d11|d3d12`，
相机和调试线测试也按该值选择设备；旧`AY_CAMERA_GPU_TEST`与`AY_PHYSICS_DEBUG_GPU_TEST`仍兼容D3D11。
新增工厂/合约检查遍历256个字节取值，并检查独立实例；纯策略另测原输入不变、幂等和保留显式错误。

首次扩大Gpu分区时，旧SkyOutput用例对失效天空仍默认期待黑色，出现9个断言失败。
该用例直接构建Pass，未经过新工厂；生产Lighting本来回退配置的清屏色，本轮未改该生产代码。
用例现显式验证黑色及两组非黑色/alpha回退，并按已存在的RGBA8清屏截断计算独立参考，仍用原1e-3容差。
Sky恢复/禁用/无源/缺图各阶段继续检查当前生产状态，不能只因缓存RT存在就采样旧天空。

两后端`assembly-native-{MaterialWorkshop,AssetReimport}-*`通过，保持Scene/预览隔离、
实际Apply/Cancel、失败保留、修复重试与保存重开。四个`assembly-quality-*`报告也通过，设备采集串行。
初次失败报告`assembly-gpu-full-d3d11.json`保留，不能当作通过或与最终结果重复累计。

## 纹理上传与采样（2026-10-22）

本轮主线完整 CPU 九组990例/16637断言通过，无跳过。
真实 `AYRenderer_PhoskiaGpu` 两后端各38例/35502断言通过，无跳过；报告为
`gpu-current-full-d3d11.json`、`gpu-current-full-d3d12.json`。格式检查覆盖4×4、5×3、1×7、7×1、
8×12、12×4，七种格式、linear/sRGB及完整层级，对独立RGBA8参考逐通道容差1e-6，单例19909断言。
超过设备尺寸上限的替换输入仍保持旧GPU各层可读，完整生命周期单例1031断言。
定向准备/解码ASan为13例/440断言，见`texture-safety-asan-current.json`。

`quality-serial-{checker,stripes}-{d3d11,d3d12}/acceptance.json` 是单独串行采集的画质/计时证据；
两个后端的checker基础级帧间差31.9089、完整三线性0；stripes三线性参考误差123.5074、各向异性16.1246。
checker的面积均值和stripes的已分辨U信号是不同的判据，不能把条纹运动本身当作闪烁。
此前`quality-reviewed-*`批次曾与GPU套件尾部重叠，其计时排除；失败与排除证据保留，不重复累计为最终通过。
空场参考在请求截图前稳定8帧，其他相机输入也先稳定后捕获；这不承诺截图API同步对应请求帧。

Editor实际两后端的AssetReimport、MaterialWorkshop、MaterialPreview均通过，分别在
`native-current-{AssetReimport,MaterialWorkshop,MaterialPreview}-{d3d11,d3d12}/`。
本轮未新增GPU OOM、设备丢失、完整Editor默认GameLoop或整引擎ASan验收。

跨模块当前进度/证据入口见[纹理采样任务](../../../AYDocs/RENDER-TEXTURE-SAMPLING-20261021.md)。
本轮主证据目录为根仓库`out/render-sampling-20261021/`，设备进程串行，CPU/Noop不替代设备证据。

| 边界 | 验证入口 |
|---|---|
| 完整/前缀/坏尾部、精确块长、尺寸/预算、采样标志、Alpha裁切 | Contracts分区的TextureUploadPlanTests |
| RRM原始上传尺寸截断/非法格式拒绝、旧缓存保留 | SceneMaterial分区的RenderAssetBridgeTests |
| 精确逐级颜色、分数LOD、min/mag、U/V寻址 | Gpu分区AYRenderer_PhoskiaGpu的resource_texture_* |
| 4×4、5×3、1×7、7×1 × 七格式 × 线性/sRGB | resource_texture_native_compressed_and_srgb_mips_preserve_channel_semantics；逐层对独立RGBA8，容差1e-6 |
| 真GPU旧数据保留、稳定ID、前缀基础级、销毁计数 | resource_texture_gpu_reload_retains_last_good_mips_and_partial_chain_is_initialized |
| 当前Editor Apply/Cancel、Scene/预览、保存重开、失败重试 | AYEditor_AssetReimportRenderTest；两后端新目录 |
| 隐式LOD的摩尔纹/斜视细节及独立计时 | AYRenderer_TextureSamplingRenderTest；输出acceptance.json、PNG序列 |

```powershell
AYRenderer_GpuTests --suite AYRenderer_PhoskiaGpu --case resource_texture_authored_mip_colors_reach_real_gpu_lod_sampling --report-json mip.json
# 上述设备测试需在独立进程设置AY_TAA_GPU_TEST=d3d11或d3d12。
AYRenderer_TextureSamplingRenderTest <新目录> d3d11 checker
AYRenderer_TextureSamplingRenderTest <另一新目录> d3d12 stripes
```

checker使用已知面积均值的Nyquist棋盘，检查固定相机轨迹稳定内部区域的相邻帧差。
stripes只沿U变化，在U信号已分辨的近处ROI对基础级双线性参考比较各向异性与三线性；
不能把物理条纹随相机移动的差异当作闪烁。无AA、Unlit Forward的受控夹具，不代表全部场景。
捕获/解码/磁盘IO不计入计时段；报告CPU提交中位/P95、有效GPU样本、设备及编码上传字节。
上传字节不是驱动实际分配，单设备微计时不作FPS提升保证。

定向ASan检查单独编译TextureUploadPlan、TextureImageLoader、Test_TextureUpload及runner，链接ASan Resource/AYTest；
外部bimg等未重新插桩，STL容器注解按既有ASan配置关闭。原始编译命令/配置钩子在证据目录，临时钩子验证后移除。
此前错误路径缺少显式bx::Error曾在普通构建触发Debug断点，失败报告保留；修复后需同时检查大头和截断小图。

## 材质工具箱与共享 program 生命周期（2026-10-20）

本阶段证据根：`out/material-toolbox-20261020/`。CPU/Noop 只证明句柄、参数和所有权契约；
Scene 与作者视图的 GPU 隔离另由真实 D3D11/D3D12 夹具检查。

| 检查 | 实际结果与证据 |
|---|---|
| Renderer 九个 CPU 分区 | 969 例 / 15951 断言，无失败、无跳过；`reviewed-cpu/` 七组 + `sparse-fixed-scene-material.json` + `reviewed-audit-fixed.json` |
| 共享资源桥 | 上述 SceneMaterial 分区85例/10226断言，内含资源桥11例/112断言；直接检查共享 token 有效性/最终回收、缺省实例值与显式参数保留 |
| 主路径表面/非均匀尺度 | 两后端各 29 断言；`normal-after.json`、`pbr-surface-d3d12.json` |
| 三种 opacity 来源及 UV | 两后端各 83 断言；`pbr-mask-d3d11.json` / `pbr-mask-d3d12.json` |
| GBuffer/Shadow/Motion 足迹 | 两后端各 79 断言；`pbr-replay-d3d11.json` / `pbr-replay-d3d12.json` |
| HDR 多预览与主后处理 | 两后端各 392 断言；`preview-display-d3d11.json` / `preview-display-d3d12.json`，原色/ACES/Reinhard 独立整数像素参考 |
| 预算/相机有限性与 lease 回收 | 2 例 / 34 断言，包含在 UI 分区；`preview-lifecycle.json` |
| MaterialTool 与周期表面 | 8 个真实 CLI/生成器用例，`material-tool-path-nul-before.log`；覆盖不写入失败、128项上限、未知字段、越界/NUL路径与周期性 |
| Native 严格作者输入与 Blend | 两后端 `native-delivery-d3d11/`、`native-delivery-d3d12/`；坏材质、PNG、缺图、坏作者 shader 配有效 fallback 均拒绝；alpha=0 真实编码参考各路径变化164553像素 |
| 导出输入一致性/保留旧输出 | `export-source-contract/acceptance.json`，真实 viewer 完成首章期间改复制库；拒绝登记该章，保留两张截图；已有输出目录不改写 |

首次 Audit 失败是旧测试仍要求 shadow mask v5，修正为实际 surface-UV v6 后单独复测通过。
`reviewed-cpu/Audit.json` 是保留的失败报告，不能当作最终通过；不累加重复运行次数。
导出还保留了一次 shaderc exit1 无诊断失败；同输入单独复现成功。预检实例现在保留到首个
实际材质接管 program，避免先释放唯一 token 再重复编译；失败仍终止导出，不静默重试。

### 复现入口

从根目录构建 `AYRenderer_SceneMaterialTests`、`AYRenderer_UITests`、`AYRenderer_GpuTests`、
`AYRenderer_MaterialTool`、`AYRenderer_MaterialDemo`。生产 shader staging 的 Native 目标依次构建，
避免多个复制任务同时改写同一输出目录。

```powershell
AYRenderer_SceneMaterialTests --suite RenderAssetBridgeTests --report-json <新报告.json>
AYRenderer_UITests --suite AYRenderer_PreviewScene --report-json <新报告.json>
$env:AY_TAA_GPU_TEST = 'd3d12' # d3d11 同样验证
AYRenderer_GpuTests --suite AYRenderer_PreviewSceneGpu --report-json <新报告.json>
AYRenderer_GpuTests --suite AYRenderer_PhoskiaGpu --case pbr_surface_controls_and_nonuniform_scale_match_across_main_paths --report-json <新报告.json>
python AYRuntime/AYRenderer/demo/TestMaterialTools.py <AYRenderer_MaterialTool.exe>
python AYRuntime/AYRenderer/demo/TestMaterialDemo.py <AYRenderer_MaterialTool.exe> <AYRenderer_MaterialDemo.exe> <新目录> --d3d12
python AYRuntime/AYRenderer/demo/TestMaterialExport.py <AYRenderer_MaterialDemo.exe> <新目录>
```

Editor 真实集成目标（均为 opt-in `EXCLUDE_FROM_ALL`，不混入 Noop CPU 分区）：

```powershell
AYEditor_MaterialPreviewRenderTest <新目录> d3d12
AYEditor_MaterialWorkshopRenderTest <新目录> d3d12
AYEditor_MaterialWorkshopRenderTest <另一新目录> d3d12 --force-sc
AYEditor_MaterialWorkshopRenderTest <又一新目录> d3d12 --multiple
AYEditor_MaterialWorkshopRenderTest <旧材质新目录> d3d12 --sparse
AYEditor_MaterialWorkshopRenderTest <目录验收新目录> d3d12 --catalog
AYEditor_MaterialWorkshopSessionRenderTest <新目录> d3d12
```

`workshop-sc-rgb-*` / `workshop-multiple-*` 验证实际 Scene 几何和纹理预览：未保存编辑的
Scene RGB 变化为0；连续四次 revision、第二预览独立编辑/关闭、主相机、保存等待显式 reload、
材质重开与 Scene 加物体保存重开的实际画面均通过。raw SC 只验证旧兼容 program 的生命周期，
不声称它实现新 PBR 表面语义。`editor-preview-shapes-*` 验证实际 ComboBox 切换形状及 undo RGBA8±1。

`workshop-sparse-before-d3d11` 首次复现了省略普通 PBR 参数的旧材质会继承预览 uniform 值。
资源桥补齐 StandardLit 缺省实例槽后，`workshop-sparse-fixed-d3d11/12` 在实际 GPU 上通过：
同时修改金属度/粗糙度/AO/发光后，Scene 变化仍为0，预览变化146235像素，保存重载后Scene变化20989像素。
CPU 新例确认缺省值独立且显式参数不被覆盖；之后 raw-SC 兼容两后端复测仍通过。

`acceptance-catalog-d3d11/` / `acceptance-catalog-d3d12/` 各加载104个实际原生 Scene，
按各自保存的 camera 渲染 Forward/Deferred，共416张 Scene 画面，另有四张空 Scene 参考。
逐帧检查 mesh/material 资源加载与对象数量、预览后的主相机不变、截图尺寸和相对空场的几何覆盖。
首版绝对亮度判据误拒深色橡胶，保留失败图片，改为与同后端/显示状态的实际空场逐像素比较；
不能通过提高黑材质亮度来迎合判据。`acceptance-editor-gpu.json` 记录该目录验收及八项编辑夹具的进程结果。

`session-shortcuts-*` 使用真实 EditorSession、资产库、Dock、Enter/Ctrl+S 路由和 GPU backend；
Scene 提取为显式测试 Host，不替代默认 GameLoop 的原生指针操作验收。Editor 材质 CPU 10例/208断言，
`authoring-created-d3d11/` / `authoring-created-d3d12/` 进一步通过真实 Session 新建材质、
无外部贴图的独立 companion shader 实际预览、新旧两个文档隔离、Enter 和 pending-field Ctrl+S、
磁盘重开及返回 Scene 像素不变。`DefaultEditorModuleAssemblyTest` 1例/13断言通过，
`AYEditorShell_Demo` 完整构建；这两个门禁仍不等同于人工原生窗口操作验收。
另复用未改变的 Shell50/1418、ProjectWorkflow35/477、AssetBrowser25/357、Framework7/108 成功报告。
AYResource 单材质独立 ASan 构建6例/316断言通过；关闭新 STL 容器注解以兼容第三方库，
仍启用地址检测，不宣称容器注解覆盖。MaterialConverter 通过，FBX 外部 fixture 跳过保留在报告中。

样例现有128份材质、411张PNG、4种原生mesh、104个Scene。截图/章节/动画帧数量是内容交付量，
不计入测试用例数。完整命令与能力边界见[样例说明](../demo/MaterialWorkshop/README.md)。

完整展示每后端60章/306张PNG，源输入541项摘要及截图解码均通过；`delivery-verification.json`
逐图保存摘要，54组状态/路径比较都有表面变化。完整展示的原程序以 `delivery-capture-viewer.exe`
保存，manifest摘要不因后来构建而改写。末次稀疏默认修复后，最新Native另捕获collection/
transparency两个构图中覆盖17表面，四状态×两路径×两后端共32图，逐像素与已有导出比较均在RGBA8±1内；
证据 `current-viewer-content/compatibility.json`。不为未受影响内容重复导出所有章节。

## 最终内容与架构收口（2026-10-19）

色板演示两管线各32帧，相邻31对均有实际变化，几何固定；`palette-performance.gif`含并排过程。
Shader整组12例/211断言通过，新增原生argv调用检查完整包成功/缺一个变体拒绝。
单独复制包后翻转阶段字节，`--check`返回1；原包保持完整。源码脚本变更实际触发完整70变体
生成32.16秒，随后无内容变化的工具时间戳触发仅验证复用，目标总耗时4.30秒，无shaderc调用。
生产包再次stage进演示目标成功，未改格式或放宽严格加载。
随后组合作者作品暴露短小数被JSON double整体比较误拒的问题，保留首次拒绝日志后修正Float32域比较。
PostProcess269例/1778断言通过，其中新增一例30断言；短小数、规范往返、范围和嵌套未知字段边界均覆盖。
CPU九组最终证据合计960例/15795断言（复用未变化组的已成功报告），不把历史重复执行次数累加。
最终inventory1003例/10分区，无遗漏重复。PaperEcho/PhosphorStop两管线各32帧，变化对数分别31/23；
前者首版纸墨对比度不足，调整双色与强度后另存最终序列。`combined-looks.gif`展示组合结果。
一键精选导出脚本实际完成11章/332张PNG，每章实际载入的look副本与清单SHA256相符；
已有输出目录另次调用被拒且清单未改。最终包`final-showcase-pack/`，总览`showcase-overview.png`，
动态合集`showcase-motion.gif`。脚本是内容导出器，不将导出帧数计为测试用例覆盖。
证据：`renderer-final-shader.log`、`shader-check-corrupt.log`、`shader-reuse-final-*.log`、
`build-final-integrated.log`、`final-cpu-reports/`、`palette-performance-summary.json`、
`combined-paper-echo-initial-refusal.log`、`renderer-short-decimal-postprocess.log`、`combined-looks-summary.json`。

## 保色分层补入（2026-10-19）

PostProcess268例通过；与保留的八组证据合计959例/15763断言。
颜色真实GPU参考扩充至两后端各150断言，覆盖ValueBands纯色比、Bayer、像素化、关闭量化、黑色和alpha。
Editor Shell50例/1418断言，验证新枚举/18套起点/偏好恢复；SDK124/53/92/530零错误警告。
CelInk/CelPaint两管线实际画面在`gallery-cel-*`，`value-bands-overview.png`给出原画与两种起点对照。
Look schema4明确拒绝旧版本使用新枚举，同时保留v1/v2/v3输入；无新pass/view/FBO/catalog项。

## 新展示内容与路径审核（2026-10-19）

天文仪/运河街区各两管线十六look导出成功，天文仪另有D3D12同内容导出；构图首轮和最终图都保留。
新增UTF-8图片/嵌套RSL往返用例。隔离环境首次临时目录写入失败，原生权限下定向与整组均通过；
SceneMaterial81例/10148断言，保留其他八组有效证据后CPU合计958例/15747断言。
Editor颜色/色板失焦精度先复现两处失败，修复后Shell49例/1407断言通过；ProjectWorkflow35例通过。
SDK124能力/53模块/91检索/530入口，0错误警告。证据前缀`native-content-*`、`io-utf8-*`、
`editor-float-focus-repro`、`editor-content-final-shell`、`gallery-observatory`、`gallery-canal-final`。

## 参考图取色（2026-10-19）

定向PostProcess整组通过；与已保存的其他八组证据合计957例/15739断言，inventory1000例/10分区。
新增四例/102断言检查alpha权重、透明排除、像素顺序、已知离散色恢复、均匀灰阶中心和失败保留。
独立RenderStyleTool从32色参考图生成8色RSL，无GPU；自定义look画廊在D3D11导出两管线成功，
逐像素全部位于嵌入色板±1字节内。Editor AssetBrowser25例/357断言、Shell48例/1395断言通过。
取色服务与已有原图/缩略图服务共用CPU解码，含UTF-8源路径、预算先验和错误图片检查。
证据：`palette-cpu-*`、`renderer-palette-postprocess.log`、`editor-palette-*.json`、
`palette-native-summary.json`、`gallery-reference-palette/`、`palette-authoring-overview.png`。

## 时间风格与动态导出（2026-10-19）

本阶段八个受影响CPU组通过，截图队列完成后SceneMaterial组再次通过；保留首阶段Audit证据，
九组合计953例/15637断言，报告复制到`temporal-cpu-reports/`。inventory996例/10分区，无遗漏重复。
TemporalFeedback真实序列在D3D11/D3D12各305断言：变化RGBA输入的混合递推、整帧/分块周期、
历史偏移、外部失效、切镜、尺寸/原点改变、失败输入与释放。主管线单次截图用例两后端各380断言，
包含两管线的保持/重置/切镜和偏移视口内外逐像素检查。首轮误在endFrame前读了上一帧pass统计，
调整用例读取时机后通过；保留失败报告。截图队列CPU四例/58断言验证陈旧/未完整文件、FIFO和有界清理。
Editor Shell47例/1379断言与ABI37门禁1例/2断言通过；SDK124能力/53模块/89检索/526入口，0错误警告。
离线shader70项，固定view remap66、UI单Pass预算141。
`--motion-gallery`在两后端各导出320张确定性动态图，全部成功；原画及四种时间效果均有实际运动，
停格采样32帧中19对发生变化，其余模式31对，符合5提交保持周期。
证据：`renderer-temporal.*`、`temporal-*.json`、`motion-d3d11/`、`motion-d3d12/`和`temporal-overview.gif`。

## 显示模拟补入阶段（2026-10-19）

八个受影响CPU组通过；保留首阶段已通过的Audit组后，合计949例/15464断言。
新显示GPU用例D3D11/D3D12各27断言，逐像素核对扫描线、RGB列mask、通道采样、alpha，
并用独立整数域参考核对确定性颗粒。共享PresentationStage调整后，颜色114与轮廓26断言的
D3D11真实用例均复测通过。GLSL/D3D三阶段编译、schema1迁移至2与严格坏文件检查通过。
Editor Shell47例通过，新增display状态/seed恢复；模式相关控件显隐不丢值。
离线目录69项，UI单Pass预算142；严格画廊两管线各十二张，新增Arcade/Broadcast有版本化样例文件。
证据前缀`display-*`、`shared-stage-*`、`renderer-display.*`、`gallery-display-final/`。


## 2026-10-19 风格工具箱主阶段

- CPU九分组948例/15354断言通过。新增slot26/view51后首轮旧表位置断言失败；更新对应
  Canonical表后，仅SceneMaterial/Geometry/LightingShadow/PostProcess四组复测并通过。
  成功的另外五组未重复；首轮失败日志与最终分区JSON均保留。
- `AYRenderer_ColorStylization`增加独立轮廓gate/图依赖、settings与严格look格式、两阶段GLSL/D3D编译。
- 真实`color_stylization_real_pass_matches_independent_pixel_and_bayer_oracle`：D3D11/D3D12各114断言；
  原像素/alpha/Bayer不回归，自定义色板最近项、双色插值与三种纸墨端点通过。
  首轮新增端点夹具尝试更新RT输入未生效，修正为每项独立初始化输入后通过；不改动待测效果来迎合夹具。
- `stylized_outline_real_pass_depth_normal_and_luminance_edges`：两后端各26断言通过，独立输入
  检查列边缘、alpha与生产失效。无效FBO附件访问在adapter入口返回invalid，不进入bgfx断言。
- Editor Shell47例/1372断言、ABI35的1例/2断言通过。完整编辑器可执行目标构建，
  不把MockRenderer控件测试写成原生UI人工验收。
- 988例/10分区清单无遗漏/重复；离线catalog68项。禁止运行时编译的两后端各20张首轮成品图
  对应差异最大1/255；含法线折痕的两条管线不宣称逐像素相等。
- 证据位于主仓`out/render-style-toolbox-20261019/`。预设最后的配色调整/实现出头文件
  另用定向回归和新画廊记录；其余已成功CPU组不为凑时长重跑。


## 2026-10-19 前向/延迟背景及 PBR 一致性修正

原 ColorStylization 画廊的前向/延迟差异不符合对照目的：ForwardOpaque 覆盖 host clear 为
硬编码灰色，Deferred 无天空时输出黑色且alpha=0；展示又混用SimpleLit与Deferred PBR。
后端PNG逐字节相等只验证D3D11/12，不能证明两条管线的背景、范围或光照一致。
新增实际主 Renderer 回归进一步发现 Forward PBR 只收到单灯 uniform，Lights 数组/
activeLightCount 未上传；背景RGB修复后，该用例仍以中心字节差26失败，补齐上传后为0。

修复在两个 frame 入口保存finite/clamp clear RGBA，Forward/Deferred 使用同一清屏量化；
场景alpha保留clear/sky/surface合成语义，coverage继续在RT3.a。Composite view0显式回到
backbuffer；效果包含完整主视口及背景，视口外UI/window保持原始清屏。
Forward opaque与透明共用已有材质环境绑定、SceneLights与阴影上传；Deferred无IBL环境项
对齐现有PBR。画廊统一内置PBR及完整法线/切线/UV。

- 最终 CPU 九组945例/15190断言通过，零skip。首轮仅旧源码顺序守卫寻找
  setViewClearRaw 而失败；更新为带 _sceneClear 的 setViewClear 后，完整PostProcess257例通过。
  成功的其它8组未重复；首轮日志/报告与最终分区JSON均保留。
- 新 `AYRenderer_PhoskiaGpu.forward_deferred_background_and_stylization_share_viewport_and_pbr`
  D3D11/D3D12各1例/214断言通过。真实PBR表面、全窗口和(13,9,128,96)嵌入视口、
  两种背景颜色、风格开/关，逐像素核对背景RGBA与Bayer相位、视口外像素及中心材质色；
  背景/外侧错误数0，PBR中心前向/延迟最大字节差0。
- `AY_SHADER_RUNTIME_COMPILE=0` 重新生成两后端各8张960×540画廊，所有图编译/draw检查通过。
  对应后端PNG字节差0；两管线背景RGBA完全相同，未风格化图最大差2/255。
  量化后少数临界像素可差一个色阶：PixelRetro最大24、LowColor/DitherRetro最大85；
  差异>1字节的像素比例分别0.0625%、0.01138%、0.13194%。这不是全局色调/作用范围差异，
  不承诺含GBuffer量化和不同专属效果时逐像素等价。
- 测试清单984例/10分区，无遗漏/重复；67项离线生产shader与AYEditorShell_Demo构建通过。
  SDK doctor124能力/53模块/85检索/523入口，0错误、0警告。

原始与最终证据：`out/render-background-parity-20261019/`，含新RGBA回归、分区报告、
两管线/两后端PNG、gallery-comparison.json、gallery-overview.png和acceptance.json。
旧画廊保留为修正前记录，不再用于证明前向/延迟视觉一致。


## 2026-10-19 ColorStylization v1 内容扩展

新增主 Forward/Deferred 的像素化、RGB 色阶量化、Bayer4 有序抖动，三组可编辑预设与
Editor schema7 用户偏好；颜色效果在 AA/ColorGrading 后、Present/Editor/UI 前。
默认及 identity 设置不声明节点/输出，无 identity draw。一个可选 RGBA8 输出、一次采样/绘制；
不含 history、固定艺术色板、蓝噪声、CRT 或时间反馈，独立 renderViews 不执行此链。

本机 Windows/MSVC Debug（同上 RTX3060/D3D11/D3D12），最终构建包括10个 Renderer
测试程序、AYRenderer_ColorStylizationDemo、AYEditorShell_Demo 和 AYEditor_UnitTests。
离线 catalog 增至67个变体；Shader组验证没有运行时 compiler 的全清单装载。
清单审计983例/10分区，无遗漏或重复；CPU最终合计945例/15190断言，零失败/skip。
首轮9组中8组通过；新增 view50 后旧的固定数组长度断言仍为62，修为63并把 style 纳入
依赖链，单独复测完整 PostProcess 分区257例全部通过，未重复成功的其他8组。

- `AYRenderer_ColorStylization`：5例/120断言，参数清洗/预设、identity gate、状态跨重建、
  AA/分级组合、资源数量、custom pipeline 和静态/view顺序，GLSL430与D3D s_5_0 编译/反射。
- `AYRenderer_PhoskiaGpu.color_stylization_real_pass_matches_independent_pixel_and_bayer_oracle`：
  D3D11/D3D12 各1例/44断言，独立 Bayer16 表和CPU量化公式核对真实 Pass 输出；
  RGB、alpha、仅像素化、无抖动量化、组合、截短块、整视口块、本帧生产与失败旁路。
- 展示程序 `--gallery --hidden --output <directory>`，另一轮加 `--d3d12`，均设置
  `AY_SHADER_RUNTIME_COMPILE=0`：各导出两种管线四组预设，预热后逐帧核对图编译及0/1次绘制。
  最终新目录16张960×540 PNG，8组对应图的后端最大字节差均为0；视觉检查及对照总览已保留。
  这只代表该固定场景，不是所有内容的跨后端图像等价证明。
- Editor Shell 最终46例/1354断言全部通过，含预设真实选择通知、手调值、两会话恢复及
  整数标签与生效参数一致；源码 ABI34 的独立1例/2断言通过。未运行全部 Editor 分区。
- `ayapi doctor`：124能力/53模块/85检索用例/523入口，0错误、0警告。

复现：构建目录 `out/build/render-phoskia-20261004`；原始失败与最终分区报告、构建日志、
GPU oracle JSON、最终画廊、逐字节对照和汇总位于 `out/render-color-stylization-20261019/`。
截图异步回调采用同一静止场景连续请求7帧等待 PNG sidecar，避免只请求一帧时仅TGA到达。
Editor测试初次受沙箱临时报告替换限制，改用正常进程运行；没有据此放宽功能断言。


## 2026-10-19 Deferred 贴花保守视锥剔除

主路径在 FrameGraph 规划前计算一次投影盒可见索引，Pass 复用该结果；独立 Pass
宿主使用同一 helper。保持源列表叠加顺序，最终 ping/pong 按可见数选择；完整原列表
纹理校验不因剔除而跳过。全屏外不分配新的私有目标、不提交贴花节点；已经分配的
MRT 在暂时屏外时保留，重新入镜核对尺寸/depth/generation。关闭/清空仍释放。
GBuffer 的 binary16 世界坐标舍入由额外世界空间 ULP 余量覆盖；没有 shader 或清单变更。

实际环境：Windows 11 10.0.26200、Ryzen 7 5800H、RTX 3060 Laptop GPU，驱动
32.0.16.1692，MSVC 14.51.36231 Debug；设备读回 vendor/device 为 4318/9568。
构建目录 `out/build/render-phoskia-20261004`。全部10个 Renderer 测试程序、
66项离线生产 shader 和 `AYEditorShell_Demo` 构建/链接通过。
清单审计977例/10分区，无遗漏或重复。CPU最终9组940例/14968断言全部通过、零skip；
贴花契约为7例/71断言，覆盖两种深度约定、六面、接触、旋转非等比、平移相机、近裁面、
相机在盒内、稳定边界、非有限数据、容量及半精度舍入。

首轮 CPU 全量为8/9组通过（114.35s）；Shader组在临时生产包写入处失败，随后发生
连带装载失败。保留原JSON和日志；同一二进制在正常非沙箱环境仅复核该组，12例全部
通过（44.31s），没有为复核修改源码或容差。成功的其他组未重复执行。
最初受限构建的 PDB C1902 同样在正常 MSVC 构建环境解决。

D3D11/D3D12 各5个独立进程用例、5375断言全部通过，零skip：

- 原有4附件材质/法线/元数据回读及借用深度、resize、资源释放回归。
- 源数5→可见2、源数2→可见1、全可见、全屏外；验证压缩后的最终FBO与叠加颜色。
- 被剔除项缺纹理或变换非法仍使混合事务整体回退，未发布部分GBuffer。
- 半精度近裁面接收点真实shader回读；CPU精确盒在视锥外也保留实际可接收的贴花。
- 真实主 Renderer 最终PNG、冷启动零私有纹理增量、resize与开关切换；TAA开启后的
  camera cut、移出和再入镜验证图编译及贴花数量。PNG逐字节对照在TAA关闭时进行。

1024×576固定探针有无贴花、16项全可见、16项中5项可见及16项全屏外四种场景。
每个场景/开关组合先预热64帧，记录32个CPU提交样本；CPU范围是可见性、图规划和
Pass提交，不含 `endFrame` 等待。GPU开启 bgfx profiler，只接受全部贴花view属于同一
GPU帧、位于当前测量提交窗口且未重复的时间戳。下表为中位数，单位ms，关→开指
`setFrustumCullingEnabled(false/true)`；GPU括号为有效去重样本数，N/A不是零耗时。

| 后端 | 场景 | 贴花draw 关→开 | CPU提交 关→开 | GPU views 关→开 |
|---|---|---|---|---|
| D3D11 | 无贴花 | 0→0 | 0.0042→0.0044 | N/A→N/A |
| D3D11 | 全可见 | 16→16 | 0.7523→0.7713 | 0.9021(9)→0.8970(10) |
| D3D11 | 混合 | 16→5 | 0.7054→0.2929 | N/A(0)→0.2099(26) |
| D3D11 | 全屏外 | 16→0 | 0.7026→0.0635 | 0.6748(22)→N/A(0 draw) |
| D3D12 | 无贴花 | 0→0 | 0.0067→0.0057 | N/A→N/A |
| D3D12 | 全可见 | 16→16 | 0.9313→0.8981 | 9.2559(31)→8.8666(31) |
| D3D12 | 混合 | 16→5 | 0.9129→0.3572 | 8.7451(31)→2.7136(31) |
| D3D12 | 全屏外 | 16→0 | 0.8867→0.0658 | 8.7786(31)→N/A(0 draw) |

两个后端各四场景、四附件完整图像对照最大字节误差均为0；真实主链三组PNG
（禁用/全屏外、混合、resize）最大误差也为0。附件比较门限原定RGBA8≤1字节、
world-position原始binary16字节完全相同；没有放宽门限。合成探针不代表完整场景帧率，
全可见时仍有CPU剔除成本和逐项全屏复制。D3D11混合基线没有合格GPU窗口样本，
不能计算其GPU提升比例；不同后端绝对计时也不能用于一般性能排名。

复现CPU与清单：

```powershell
cmake --build out/build/render-phoskia-20261004 --target AYRenderer_TestGroups AYEditorShell_Demo --parallel 4
ctest --test-dir out/build/render-phoskia-20261004 -L '^renderer-full$' -LE '^gpu$' --output-on-failure -j 1
pwsh -NoProfile -File scripts/tests/verify-module-test-inventory.ps1 -Module AYRenderer -BuildDirectory out/build/render-phoskia-20261004
```

设备用例使用 `AYRenderer_GpuTests.exe --suite AYRenderer_PhoskiaGpu --case <name>`，
每项独立进程，`AY_TAA_GPU_TEST=d3d11` 或 `d3d12`，不得与shader构建并发运行。
固定性能/图像入口为 `deferred_decal_culling_fixed_scene_image_and_timing_baseline`；
主链入口为 `deferred_decal_main_renderer_graph_bypass_resize_and_final_pixels`，
`AY_DECAL_CAPTURE_DIRECTORY` 可指定PNG输出目录。
本次完整runner、CPU/设备JSON、原始失败、计时/p95/帧号和PNG保留于
`out/render-decal-culling-20261019/`，最终汇总为 `cpu-summary.json` 与 `gpu-summary.json`。
`initial-*`/`gpu-summary-initial.json` 是早期取样，不能替代带提交窗口过滤的最终数据。
SDK doctor：123能力/53模块/83搜索用例/519已文档化入口，0错误0警告。

额外观察：既有 `validDeferredDecalTransform` 对45°Z旋转加scale(4,.25,1)的正交正尺度
矩阵返回false；首轮旋转fixture因此未满足准入前提。最终使用既有校验接受的30°旋转
验证剔除；该45°准入问题需单独修复，本次未修改AYMath或投影体准入算法。
## 2026-10-19 可见发光口

FixtureEmitterTests新增4项，Contracts总153/153、1025断言通过。FixtureEmittersGpu在
D3D11/D3D12各2/2、59断言，无跳过；使用主Deferred最终LDR回读验证方向、完整/部分
遮挡、烟雾与Bloom。正面/侧面/背面/完全遮挡/半遮挡红通道总量为
28560/630/0/0/14280，不放宽半遮挡阈值。极小口径、源HDR上限后的介质衰减与
256灯预算另由CPU覆盖。Shader12/12、208断言，68变体离线与无编译器加载通过。
清单1003项/10分区无遗漏/重复。设备测试需显式AY_EMITTER_GPU_TEST=d3d11或d3d12，
默认跳过，不冒充硬件验收。当前压力性能和已知近似见项目灯光系统文档§8；未人工查看图片。
日志out/emitter-gpu-d3d11-verified.log、out/emitter-gpu-d3d12-verified.log、out/emitter-contracts-full.log。

## 2026-10-18 聚合Point与光通变焦

Contracts149/149、964断言；Shader12/12、208断言通过，68生产变体重新烘焙并通过
无编译器加载测试。D3D11 GPU44项中41通过、3项既有独占设备夹具跳过，17366断言通过；
D3D12 Phoskia31/31、11027断言通过。新增CPU覆盖RGB/中心、乱序、预算/相同亮度、
65536点上限及非法值原子失败；GPU覆盖实际聚合器输出到表面Pass和表面/体积光通强度。
Renderer清单997项/10分区无遗漏或重复。性能及后续限制见项目灯光系统文档§7，
本轮未做人工图片验收。日志：out/lighting-stage8-contracts-final.log、
out/lighting-stage8-shaders.log、out/lighting-stage8-gpu-d3d11.log、out/lighting-stage8-gpu-d3d12.log。

## 2026-10-16 聚光灯数组 / 发光点演出

Windows Debug：合入主线诊断显示后，renderer-full非GPU9组通过（124.72s）。
D3D11/D3D12的AYRenderer_PhoskiaGpu各27用例9862断言通过，无跳过。
新增实际LightingPass双光相加、VolumetricPass消光只应用一次与双光散射、scissor不泄漏；
透视Spot按独立相机基向量和CPU积分逐像素核对，覆盖AYMath通用逆矩阵修复。
截图回归覆盖异步TGA晚到后转换PNG；67项生产shader全部编译并在两设备路径无compiler加载。
日志：out/lightshow-renderer-integrated.log、out/lightshow-integrated-d3d11.log、out/lightshow-integrated-d3d12.log。
两后端耗时61.12s/93.27s，清单981用例/10完整分区，无遗漏或重复。
实际画面与Release性能记录见[演出系统](../../../AYDocs/LIGHTING-SHOW-SYSTEM.md)。

## 2026-10-06 Deferred Decal v1 / 三项新增能力集成收尾

VS2026 Insider Debug，同 out/build/render-phoskia-20261004，production66变体构建部署。
Test_DeferredDecal：复制所有权/finite/奇异/镜像/剪切拒绝/16容量、无设备不分配、
FG替换GBuffer依赖、禁用节点裁剪、晚到Decal契约拒绝；Motion测试明确Decal插入位置。
renderer-full非GPU9入口全过118.60s（out/render-decal-final-cpu.log）；
EditorShell1用例58断言通过（out/render-decal-final-editor.log），SDK doctor0错误0警告。
D3D11/D3D12完整PhoskiaGpu各22用例8603断言全过，无skip，分别59.56s/89.23s，
含66项生产包无compiler装载及SSR/Volume/Decal真实源回读：out/render-features-final-d3d11.log、d3d12.log。
Decal四附件探针每设备1390断言：颜色/alpha/normal/roughness/metallic更新，
盒外/天空/背面保持原值；world/AO/model/emissive/coverage保持，FP32探针容差1e-4。
随后补强实际Pass深度/生命周期探针，每设备531断言全过（out/render-decal-depth-d3d11.log、d3d12.log）：
两项重叠ping/pong、resize/generation重建、缺纹理原GB回退、禁用无draw、
私有颜色计数+8并在销毁后归零增量；原depth数值0.37在draw/重建/销毁后不变。
实际RGBA8合成颜色检查容差.008，明确包含目标量化，不把FP32测试当RGBA8精度。
用户场景视觉尚待验收；Decal ECS/保存、大量贴花优化与体积透明仍为首版限制。

## 2026-10-06 Volumetric v1

VS2026 Insider Debug，同 out/build/render-phoskia-20261004；production65变体编译部署。
Test_Volumetric：默认关闭、finite settings、FG依赖/关闭无输出、非法下游顺序拒绝。
真实生产 Phoskia 源在 D3D11/D3D12 每设备1用例511断言通过：零密度、全局消光
和单次散射、局部AABB截断/未命中、阴影遮挡、HG正负方向关系；逐像素1e-4。
日志 out/render-volume-final-d3d11.log / d3d12.log；EditorShell1用例56断言通过，
out/render-volume-editor.log；不替代用户视觉验收。首版无体积透明/独立历史。
最终 renderer-full 非GPU9入口全部通过，out/render-volume-final-cpu.log。

## 2026-10-06 SSR v1

VS2026 Insider Debug 构建目录 out/build/render-phoskia-20261004。
新增 Test_Ssr 覆盖 finite settings、默认关闭、HDR graph、append-only ID/view order。
Test_PhoskiaGpu 的两个 SSR 探针验证 sky/rough/miss 保持原 HDR，以及不同表面命中、
confidence 和 BRDF/IBL 镜面替换的逐像素对应，容差1e-4。
内置目录64变体，EditorShell部署和面板存在性回归；不把这些证据称为用户视觉验收。
早期 shader 材质 return 覆盖诊断、loop implicit sample 的大程序问题均已修正；
loop 改为 explicit LOD0；矩阵改用独立场景 uniform 避免 frame builtin 重名。
最终日志 render-ssr-final-*：D3D11/D3D12 每设备2用例441断言均通过（含非原点相机）；
production64变体构建与 EditorShell1用例53断言通过。renderer-full 非GPU9入口全过。

## 2026-10-05 Phoskia生产稳定性收尾

当前语言、版本与生命周期权威已归并到[AYShader design当前契约](../../AYShader/design.md#current-contract)，
下方旧阶段计数/格式只适用于记录当时。使用方法集中于AYShader README，SDK同步当前契约。

本轮Debug Shader full345/2562、fast152/716，非GPU renderer-full的9个入口全过（119.15s）。
Release Shader同一用例/断言集合全过（两个入口37.65s）；Release Renderer Shader入口
通过（28.35s），包括63项目录、缺失/更名/宏不匹配、8次装卸和陈旧句柄拒绝。
标准Debug与Release各重建63个windows/s_5_0 engine变体，schema4，不改变生成语义身份。
真实D3D11/D3D12在Debug和Release独立进程串行全部通过，每组27用例/12334断言，零跳过；
设备JSON和日志保留在out（Release D3D11另补独立报告，避免CTest后续覆盖前一组统计）。
新增迁移包压力用例每设备16次组合draw/Compute/回读/释放，维持1e-4容差，资源计数恢复基线。

CPU生命周期/热重载套件每轮2用例/829断言；Release连续10轮全部通过，110.54s、
8290断言，共320次包读取及320个生命周期循环（含重复安装）、160次离线warm、120次失败/修复循环。
保留完整每轮报告、基线JSON和失败即停汇总：out/phoskia-stability-soak-release。
复现runner为AYShader/tools/ProductionStability.ps1，Repeat1..50，输出目录必须新建。
这不是小时级soak，不代替Editor人工视觉、完整游戏导出或其他平台验收。

性能参考（Release压力第1轮，同机CPU/Noop微基线，单位ms；p95 nearest-rank）：

| 路径 | 样本数 | median | p95 |
|---|---:|---:|---:|
| 离线warm（含compiler/include内容身份） | 16 | 103.011 | 109.530 |
| 包读取/校验 | 32 | 3.443 | 3.987 |
| 安装 | 32 | 0.076 | 0.100 |
| 生产包acquire | 32 | 0.034 | 0.054 |
| 释放后重新获取 | 32 | 0.035 | 0.052 |
| 开发文件cache hit | 12 | 9.793 | 11.542 |
| 热重载修复编译 | 12 | 254.090 | 285.568 |

生产包命中无需启动compiler；开发warm仍做源码/依赖内容检查，不应拿开发编译接口替代
已保留句柄的逐帧submit。数据是小程序微基线，不设跨机器绝对耗时断言；完整样本统计见汇总。
gpuLifecycleRoundTrip含CPU提交、帧同步、GPU回读和销毁等待，不是纯shader GPU时间。

| 4×4设备生命周期端到端（16样本） | median ms | p95 ms |
|---|---:|---:|
| Debug D3D11 | 13.671 | 15.319 |
| Debug D3D12 | 16.636 | 20.038 |
| Release D3D11 | 10.192 | 11.169 |
| Release D3D12 | 5.528 | 6.305 |

原始设备输出：out/phoskia-stability-debug-d3d11-raw.log、debug-d3d12-raw.log、
release-d3d11-final.log、release-d3d12-raw.log（后三者同phoskia-stability-前缀）。
该表包含首次生命周期，不排除首样本；不能用于比较backend纯GPU吞吐或完整帧预算。

本轮实际修复成功命中后仍保留旧诊断的问题；新覆盖门禁的负向用例发现并修复模块prepare
重置先前job错误的情况。首次失败日志仍保留，最终全量以stability系列日志为准。
SDK doctor 73能力/53模块/172符号、0错误/0警告；本轮相关README/design/SDK链接检查0死链。
既有偶发并行程序获取失败首次根因仍未证实，不能把串行全过写成已经定位；SDK维持beta。

## 2026-10-05 默认 Phoskia 生产链路

最终shader-full/renderer-full/resource-full/editor-full共19个CTest入口全部通过（231.88s），
Editor主套350用例/6064断言；动画作者core三入口补建后通过。设备回读单独运行，不能把
便携CTest的GPU opt-in跳过当作D3D设备验证。SDK doctor 73能力/53模块/165入口，0错0警告。
最终CPU日志：out/phoskia-production-roles-cpu.log。

### 本轮验证

- ShaderBuildTests：5用例/115断言，包括严格缺变体、同源不同label命中、释放后重建、
  冲突事务回滚、模块版本及超过260字符的包路径。
- Test_ShaderProduction：实际63项清单编译、普通source/raw acquire复用和旧清单拒绝。
- D3D11、D3D12独立进程各23用例/9174断言，无跳过。新增完整清单用例通过生产
  configureShaderPool读取包及policy（编译器设为不存在路径），普通acquire无编译miss；
  作者角色用搬迁后的相对shaderc/include编译新预览并像素回读，缺工具拒绝；原有UBO、
  Compute、模块/函数、pixel readback、TAA与实例化门禁保持原容差。
  日志：out/phoskia-production-roles-d3d11.log（75.232s）、
  out/phoskia-production-roles-d3d12.log（146.595s）。
- ProjectBuild默认程序/模块发布与失败保留旧输出通过；Windows深目录失败已修复。
- 标准编辑器构建及install规则成功生成schema3 engine包，安装后63项stage摘要逐项匹配。
  编辑器使用authoring.policy并附带相对compiler/include；游戏production.policy仍严格。
  一次整合重建曾受并行AYScriptTasks.cpp错误阻挡，随后成功重建Editor与Entity；之后另一次
  AYEditorStage重建遇到并行AYLogiaPackage.cpp编译错误。install规则用最近成功构建验证，
  不等于持续变化中AYScript最新源码全树验收；未修改或提交该模块。

首轮同时运行CPU与GPU时，旧TAA rejection探针曾在程序获取阶段失败；独立单例1004断言
和后续D3D11/12整组均通过，未改变测试容差。尚未单独证实首次失败根因，不将并发编译
竞争写成确定结论；设备回归保持隔离、串行。CPU入口中的可选外部FBXfixture仍可能跳过。

## 2026-10-04 Phoskia 第6项：作者工具/编译隔离

共享生产frontend/IR与BGFX Emit的报告核心、CLI analyze 和现有Editor DSL面板已接入。
Compiler类型环境隔离、legacy语义失败和默认资源options有回归。最终CPU12入口全过
（144.47s，Shader152/716+336/1583）；Editor349/6033及独立装配入口通过。
D3D11/12各9 suites/22 cases/8958 assertions，无skip、容差不变。
inventory Shader488/2、Renderer943/10、Editor350/2；CLI JSON成功/失败路径通过。
初次Editor inventory空数组来自未构建DefaultEditorModuleAssemblyTest，补建后核对通过，
不是被跳过的测试。记录日志out/phoskia-expansion-6-verified-cpu.log、editor-full.log、
editor-assembly.log、d3d11/d3d12.log、inventory-* 与cli-verified/invalid.json。
源码分析成功不代表GPU验证，报告成本不是GPU时间；作者功能限制见AYShader README。

## 2026-10-04 Phoskia 第5项：Compute 实际调度

D3D11/12 各9 suites /22 cases /8958 assertions，无 skip、容差未放宽。
Test_PhoskiaGpu 增加 compute→graphics、shared/global atomicAdd、屏障、组数量
与 RGBA32F image 回读，以及线程/所属池/过期 token/参数预算拒绝。
CPU 12入口最终串行回归中 Renderer10入口与 Shader fast 通过；Compute golden
输出变化（include/context 与空行）修订后，Shader integration 单独重跑见
out/phoskia-expansion-5-shader-verified.log（331/1533全过）；全量基底 closed-cpu.log。
首次并发 CPU/GPU 中 Bloom shaderc 无输出返回失败，单独和后续串行均通过，
不将其归因为已证实的算法问题。完整设备日志为 expansion-5-final-d3d11/d3d12.log。
inventory Shader483/2、Renderer943/10；SDK doctor0/0，独立 Editor 重建完成。
Windows compiler 替换回归的 exe 写入允许最多250ms短时重试，失败不继续构建，
不重试 shaderc，也不放宽缓存 miss/新 key 的断言；实际 golden 尾部空行保留。

## 2026-10-04 Phoskia 第 4d 项：模块依赖

D3D11/12 完整设备组各 9 suites / 20 cases / 8721 assertions 通过，无 skip，
回读容差不变。新增 module 用例证明同时间戳内容修改取得新编译结果。
日志 out/phoskia-expansion-4-module-d3d11.log / d3d12.log；CPU 全量日志为
out/phoskia-expansion-4-module-final-cpu.log。模块 inventory Shader 479/2、
Renderer 941/10，SDK doctor 0/0。Compute dispatch/readback 与作者工具还在实施。

## 2026-10-04 Phoskia 第 4c 项：值结构体

CPU 12 入口通过（124.42s）；D3D11/12 Phoskia 组各 9 cases / 2242 assertions，
无 skip、原容差不变。struct 用例验证 constructor、字段改写、helper 参数/返回及 bool。
日志 out/phoskia-expansion-4-struct-final-cpu.log、同前缀 d3d11/d3d12.log；
inventory Shader 474/2、Renderer 940/10，SDK doctor 0/0。前一阶段完整 9-suite
设备组已通过，module include 与 compute 设备 closure 尚未完成。

## 2026-10-04 Phoskia 第 4b 项：纯数学函数

完整 CPU 12 入口通过（125.56s）；D3D11/12 各 9 suites / 18 cases / 8513 assertions，
无 skip、原容差不变。Helper 测试回读 vertex/fragment 前向调用、提前 return、
循环、bool/uint 及生成符号碰撞；不会用 shaderc 成功代替设备执行。
日志 out/phoskia-expansion-4-function-final-cpu.log、同前缀 d3d11/d3d12.log。
inventory Shader 473/2、Renderer 939/10，SDK doctor 0/0；struct/module 后续实施。

## 2026-10-04 Phoskia 第 4a 项：受限循环

D3D11/12 独立 Phoskia 组各 7 cases / 2039 assertions，无 skip；索引赋值、
嵌套循环、uniform/local 遮蔽回读通过。完整 CPU 12 入口通过，Shader 152/716 +
319/1289；清单 Shader 471/2、Renderer 938/10，SDK doctor 0/0。
日志 out/phoskia-expansion-4-loop-final-cpu.log 与同前缀 d3d11/d3d12.log。
普通 CTest 的 GPU skip 不算设备证明。函数/struct/module 仍待后续语言阶段。

## 2026-10-04 Phoskia 六项扩展：第 3 项离线构建与预热

- 完整 D3D11/12 各 9 suites / 16 cases / 8243 assertions 通过；离线包 GPU 用例
  覆盖混合 std140 绑定、读取后预热、不依赖 shaderc 及真实 backend 检查。
- 添加 binding 摘要后的最终回读：Phoskia 两设备各 6 cases / 1904 assertions 无 skip，
  原 1e-4 容差不变。包读取失败不产生部分可用列表，预热失败释放本次句柄。
- CPU 12 入口全部通过；最终 Shader 摘要回归 152/716 + 317/1255。Shader 清单
  469 / 2 partitions，Renderer 937 / 10 partitions；SDK doctor 0 errors/0 warnings。
  CLI 实际构建示例 2 variants；独立编辑器重建成功，未替换运行进程或做视觉验收。
- 日志：out/phoskia-expansion-3-verified-cpu.log、out/phoskia-expansion-3-integrity-tests.log、
  out/phoskia-expansion-3-verified-d3d11.log、out/phoskia-expansion-3-verified-d3d12.log、
  out/phoskia-expansion-3-integrity-d3d11.log、out/phoskia-expansion-3-integrity-d3d12.log、
  out/phoskia-expansion-3-integrity-cli.log。工具/API 边界见 AYShader README/design。

## 2026-10-04 Phoskia 六项扩展：第 2 项缓存与热重载

- CPU 12 个 CTest 入口全通过（118.23 秒）：Shader 152/716 + 314/1175，Renderer
  10 组；Shader 清单 466 cases / 2 partitions。大写 .SH 同时间戳变更补测也通过。
- D3D11/12 Phoskia 各 5 cases / 1827 assertions 实际回读，无 skip；本阶段只改变
  缓存/修订/监听/诊断，不改渲染算法。第 1 项完整设备组各 15/8166 的门禁保持独立记录。
- SDK doctor 0 errors/0 warnings；独立编辑器重建成功，未替换运行中的旧进程。
- 日志：out/phoskia-expansion-2-final-cpu.log、out/phoskia-expansion-2-d3d11.log、
  out/phoskia-expansion-2-d3d12.log、out/phoskia-expansion-2-upper-case.log、
  out/phoskia-expansion-2-sdk.log；红测试及设计边界见 AYShader/design.md。

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
- Contracts 超时 30 秒，integration 组超时 180 秒，Gpu 超时 600 秒。
  integration 组及涉及后端的专项门禁共用 `renderer-backend-fixtures` 资源锁；
  外部暂存路径尚未全面审计，不承诺这些组可安全同时运行。Gpu 使用 RUN_SERIAL。
  完整实机D3D12套件已测到341–352秒，因此将其进程watchdog从240秒调整为600秒；
  未降低任何画面断言。此超时是门禁保护，不是性能目标；GPU默认仍显式跳过。
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

## 2026-10-16 聚光灯光束修复回归

有限出光口、核心/外场衰减和解析光束弦段的最终验证：
`AYRenderer_ContractsTests` 139/139、864断言；`AYRenderer_ShaderTests` 12/12、206断言，
含67个生产shader变体的无编译器预热检查。模块清单983用例、10分区，无遗漏/重复。
真实D3D11全GPU组40项中38通过、2项因需独占D3D11设备跳过，16672断言通过；
最终归一化几何探针另在D3D11单项重跑，471断言通过。
真实D3D12 `AYRenderer_PhoskiaGpu` 28/28、10333断言通过；
其中几何探针验证锥体命中/未命中、核心、半影和近线性交点，
透视探针以独立高密度CPU积分比较实际散射像素。
D3D12单灯空中场景也完成实际渲染与截图。
性能场景、配置和截图见[灯光演出系统](../../AYDocs/LIGHTING-SHOW-SYSTEM.md)。


2026-10-19: PhotometryTests cover bounded LM-63 Type C parsing, symmetry, physical units,
color/flux normalization and Legacy preservation. FixtureEmittersGpu (AY_EMITTER_GPU_TEST=d3d11/d3d12)
adds real main-view IES surface/volume energy and directional readback. Contracts155/1193,
Sequence105/3871, shaders12/208 and each GPU backend3/93 passed; owned inventory1006/10.


2026-10-19 optical stage: FixtureOpticsTests independently integrate prism angular energy,
check focal pupil geometry and conservative lobe bounds. LightShowOpticsTests verify continuous
focus/rotation and stepped topology. Real D3D11/D3D12 readback verifies rotating lobe locations
and defocused image-edge contrast. Contracts157/1223, Sequence106/3888, shaders12/208,
each GPU backend4/143, Studio optics8-frame smoke passed. SDK doctor126/53/83/542,0 errors/warnings.


2026-10-19 camera stage: Contracts158/1236, PostProcess252/1357, Sequence106/3888;
D3D11 and D3D12 each5/184 cover zero-Bloom glare, rotation, ghosts, source occlusion and disabled
pixel identity. Production shaders12/208 passed in isolation after one transient shaderc process
initialization failure in a concurrent run. SDK127/53/86/544 passed; owned inventory1011/10.


2026-10-19 housing stage: Contracts160/1355, both D3D backends6/238 including hood/leaf
opaque occlusion with dark-body baseline subtraction. Studio housing/shade/disabled smokes passed;
invalid dimensions exit2. SDK127/53/87/545 passed; inventory1014/10. No production shader
source changed; shared PBR/particle variants remain68.


2026-10-19 PCSS stage: Contracts161/1373, LightingShadow137/1478, Sequence107/3902,
production shaders12/208 (68 variants); each D3D backend7/306. GPU partial-shadow pixels:
contact0, legacy hard0, large pupil350, small pupil90; volume hard/soft difference48 and bounded by
unshadowed energy. Studio new/legacy smokes passed. SDK127/53/88/545 and inventory1016/10 passed.


2026-10-19 transparent/SSR stage: Contracts163/1386, PostProcess252/1357, Sequence107/3902,
Shader12/209 (69 production variants, no-compiler prewarm). Each D3D11/12 FixtureEmittersGpu9/395;
transparent alpha increments177/89, rear60, stack208 vs207 expected, Gobo/IES and disabled identity.
Each backend real no-IBL SSR1/27, scene1/2 -> reflected1.5010/3.0020; both legacy SSR cases passed.
Studio glass/reflections smoke and invalid-budget rejection passed. SDK127/53/88/545 clean;
Renderer inventory1021/10 clean. SSR remains on-screen opaque only; no refraction or indirect bounce yet.


2026-10-19 single diffuse screen-bounce stage: Contracts164/1395, PostProcess252/1357,
Shader12/209 (69 variants/no-compiler prewarm). Each D3D11/12 indirect real pass1/86,
plus three existing SSR cases; unit/double1.3193/2.6387, half-strength1.1592,4ray/32step1.0938.
Metal/black/uncovered preserve input; zero light stays zero. GI-only and combined Studio8-frame
smokes passed, invalid5rays exit2. SDK128/53/88/547 clean, inventory1023/10 clean.


2026-10-19 reduced volume / housing batches / profiler: Contracts166/2026, PostProcess252/1359,
SceneMaterial79/9886, UI114/377, FrameGraph41/159, Sequence107/3902, Shader12/210(70 variants,
no-compiler prewarm). Each D3D backend low-res extinction/checker/depth/noise1/674,
main volume1/18, housing instance+native profiler1/17;9 prior optics/395 suite also passed per backend.
Housing96objects: backenddraw102->9, instanced96, RGBdifference0. Volume energy110260/108787/109872.
Full scene CPU sample45.501->20.644ms after contiguous housing meshes; downsample is still opt-in.
Isolated64light volume GPU2.824/1.644/.611ms for divisors1/2/4. Combined45-frame Studio effects
smoke and invalid3divisor rejection passed. SDK129/53/91/551 clean; inventory1028/10 clean.


2026-10-19 局部雾重建补充：重建消光的4点密度规则同时应用局部盒.75..1柔边权重与动画噪声，
修复局部设置下散射和消光密度不一致。常量全局路径保持原解析式。
真实GPU回归新增局部/全局×静态/动画×1/2/4分辨率对照，两后端各1项1298断言通过。


2026-10-19 froxel阶段：Contracts167/2094、PostProcess252/1361、SceneMaterial79/9965、UI114/377、
FrameGraph41/159，Shader12/212（72生产变体及无编译器预热）；清单1031项/10分区。
D3D11/D3D12各解析系数网格1项155断言、主Deferred1项46断言通过。
主视图局部动画雾ray120311/grid120156，图案+分束91172/87868；预算与atlas resize均验证。
45帧组合Studio预览实际160x64x32=327680cells、4表面/4体积/8透明draw，非法tile3退出2。
SDK129能力/53模块/91检索/551符号，0错误0警告；未查看图片。
原FixtureEmittersGpu 9项/395断言在D3D11与D3D12均通过。


2026-10-20 独立体积历史最终验收：Contracts168/2119、PostProcess252/1363、SceneMaterial79/10042、
UI114/377、FrameGraph41/159、Shader12/213（73生产变体和无编译器加载）通过。
D3D11/D3D12各历史kernel1/619、主视图1/29（恒定像素差0、熄灯0、切片resize/显式重置/关闭）通过；
历史关闭的原网格解析1/155和主视图光学/局部动画雾1/46在两后端亦通过。
真实关闭路径发现FrameGraph仍返回未重导入的旧external句柄，已修复并加入CPU/GPU回归。
45帧Studio玻璃/图案/PCSS/镜头组合预览实际160x64x32、327680cells、historyUsed=true，退出0。
SDK129能力/53模块/92检索/552符号，0错误0警告；Renderer1034用例/10分区无遗漏重复。
.lshow仍schema7，无图片查看。后续雾密度编排、点阵与Editor及频闪积分仍需继续。


连续写入阶段验收：Renderer Contracts169/2211、Sequence108/3920通过；新增可选渲染桥成功/失败/关闭计时用例。
点实例/CPU四边形真实GPU对照D3D11、D3D12各1项24断言通过；Renderer清单1035项/10分区，Sequence108项/3分区无遗漏重复。
相同90帧热身后样本，CPU Presenter7.6742→3.3616ms、点批次6.6615→2.3455ms，整帧CPU20.4229→16.5421ms。
保持10000个可见实例；GPU均值10.0923/8.3168ms仅记录同机样本，不将其单独归因于CPU写入修改或承诺帧率。
默认未开启计时时45帧预览分段样本0/各段0，退出0。schema7未变，无图片查看。
SDK门禁：129能力/53模块/93检索/554符号，0错误0警告。


2026-10-20 schema8雾编排最终验收：Contracts171/2242、PostProcess252/1363、SceneMaterial79/10042、
UI114/377、FrameGraph41/159、Sequence114/4019、Shader12/213（73生产变体/生成打包/无编译器加载）通过。
整包循环重载测试补齐bgfx延迟释放帧边界，八轮重载/旧token失效仍验证；临时诊断快捷路径已移除。
D3D11/D3D12各density atlas1/702、三种积分路径/双灯/resize/零场1/541通过；
既有局部/全局×动态/静态×1/2/4重建1/1298、legacy散射/局部盒/阴影1/737、旧光学9/395在两后端均通过。
隐藏原生Fog画板创建16³、拖动/一次Undo/Redo、物理key与编解码两后端通过；D3D12同时froxel/history=true。
独立Core/EditorCore构建与fog/key/brush合约通过，无Renderer/音频/AssetIO。
清单Renderer1038/10分区、Sequence114/3分区无遗漏重复；SDK129能力/53模块/95检索/561符号，0错误0警告。
最后一次相同90样本64灯/1万点/1920x828默认无密度图配置：Presenter3.1807ms、点批次2.2276ms、
整帧CPU15.8719/GPU9.1274ms、10000可见实例。是本机Debug样本，未据此承诺普遍帧率。
资源格式登记与SDK写schema8、旧1..7空雾迁移同步；没有查看图片或连接演出硬件。

2026-10-20 continuous-point validation: Contracts175/2321, Sequence120/4157,
SceneMaterial79/10042, PostProcess252/1363, Shader12/213 (73 production variants) passed.
D3D11/D3D12 coherent-perspective contact probes each1/376; legacy point surface each1/139.
D3D12 combined Studio45 frames:1000 visible points,16 lights,froxel/history,16.67ms shutter.
Inventory: Renderer1043/10 partitions, Sequence120/3. SDK129/53/97 search/565 symbols,0 errors/warnings.
Debug/D3D11 1920x828,64 fixtures/10k points,16 aggregate lights,80 surface/12 volume/4 shadow,
24 steps,90 samples: initial smooth aggregation25.8929ms; cached smooth5.3723ms.
Final hard/smooth/contact aggregation4.1573/5.3723/7.3837ms, GPU11.4194/12.6907/15.6380ms.
All retain10k visible instances/16 lights. Process timing varies; no universal FPS claim.

2026-10-20 GPU point acceptance: D3D11/D3D12 each2 cases/231 assertions,
all full-image CPU/GPU comparisons max difference0, including dense65536 last output,
partial257 tiles, oblique camera, subpixel energy, black frames, budgets, path switch,
readback-in-flight owner destruction and device shutdown. Six baked stages built.
Contracts177/2343, SceneMaterial79/10401, UI114/377, FrameGraph41/159,
PostProcess252/1373, Sequence120/4157, Shader12/213(73 standard variants) passed.
Inventory1046/10 and120/3; SDK129 capabilities/53 modules/99 searches/566 symbols,0 errors/warnings.
Same Debug/D3D11 90 samples: point CPU1.9881->.9917ms; GPU compute.7098ms.
Frame CPU23.5177->22.0014ms, GPU12.0987->12.6955ms, a measured tradeoff.
Async sample10000/10000; D3D12 combined sample1000/1000 with froxel history/contact/shutter passed.
