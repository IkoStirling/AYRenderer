# FrameGraph MVP Cutsheet（中期）

> 与 [`short-term-plan.md`](short-term-plan.md)、[`renderer-pass-roadmap.md`](renderer-pass-roadmap.md)、[`post-process.md`](post-process.md) 配套。  
> 日期：2026-07-23 · **现在只写契约，不写实现**。短期计划禁止开工本文件范围外的代码。

## 1. 要解决什么

当前管线靠显式 `RenderPipelineDesc` 槽位 + `PassExecContext` 借用指针。  
这对「拓扑固定、十个以内 Pass」够用；对 **半分辨率临时 RT / Ping-pong / 效果开关裁剪** 会复制粘贴爆炸。

FrameGraph MVP 只解决：

- 命名资源（谁写、谁读）
- 按开关裁掉未用 Pass / 未分配 RT
- Transient 同帧复用（简单 alias）

**不**解决：跨帧自动 history、多队列、替换整个 `RenderPass` 体系、自动 barrier 优化。

## 2. 范围（红线）

### 2.1 先盖这段（唯一允许迁入 FG 的区间）

```
[LightingOutput / Transparent 之后] → Bloom* / 其它 PP 效果* → Final PostProcess → UI
```

| 在范围内 | 不在范围内（禁止第一刀迁入） |
|----------|------------------------------|
| Bloom downsample / blur / upsample | ShadowPass |
| 将来的 SSAO / DOF / ColorGrade（若独立 Pass） | GBufferPass / LightingPass |
| Final tonemap+gamma（可仍叫 PostProcessPass） | SkyboxPass |
| | TransparentPass |
| | UIPass |

稳定的 Deferred 前半段继续用手写槽位；FG 只吃「Lighting 之后的图像处理链」。

### 2.2 与现有 API 共存

- 宿主仍调用 `configurePipeline(makeDeferred())`。
- FG 是 **Renderer 内部** 在 `Transparent` 与 `UI` 之间的子调度器，不是 Editor 新管线 API。
- 现有 `setPostProcessGamma` / `TonemapMode` / `Exposure` 继续驱动 **Final** 节点。

## 3. 最小对象模型（实现时照此切）

```text
FgResourceId   — 稳定字符串或枚举（SceneColor, BloomMip0, …）
FgTextureDesc  — format / scale(full|half|quarter) / transient?
FgPassDesc     — name, reads[], writes[], enabled, execute callback 或 RenderPass*
FrameGraph     — addPass / importExternal / compile / execute
```

### 3.1 导入的外部资源（不由 FG 创建）

| 名 | 来源 | 说明 |
|----|------|------|
| `SceneColor` | LightingOutput（或 Forward sceneFbo） | PP 链输入 |
| `SceneDepth`（可选） | GBuffer depth / scene depth | SSAO/DOF 以后再用；MVP 可不 import |

### 3.2 FG 自建 transient（MVP 示例）

| 名 | 尺寸 | 用途 |
|----|------|------|
| `BloomBright` | half | 阈值提取 |
| `BloomBlurA` / `BloomBlurB` | half | ping-pong |
| `PpComposite`（可选） | full | 若 Final 需要离屏再 blit；也可 Final 直接写 backbuffer |

### 3.3 Final 节点契约

- 读：`SceneColor` +（若 Bloom 开）`BloomBlur*`  
- 写：backbuffer / composite 目标（与今日 `PostProcessPass` blit 目标一致）  
- Shader：现有 ACES + gamma；Bloom 用 `bloomStrength.x` 或独立 sampler 合成  

## 4. Compile 规则（必须简单）

1. `enabled == false` 的 Pass **不分配** 其私有 write 资源（若无其它 Pass 读）。  
2. 被裁掉的 Pass 的下游若仍需要输入：用 **边旁路**（Bloom off ⇒ Final 只读 SceneColor）。  
3. 同尺寸、生命周期不重叠的 transient 允许 alias（MVP 可先不做 alias，只做「按需创建」）。  
4. viewId：继续由 Pass 内约定递增；FG 不发明第二套 view 分配器（避免与 Shadow/GBuffer 冲突）。  

## 5. 验收（中期开工时用）

1. Bloom off：与今日 Final-only 字节级观感一致（或文档声明的可接受差）。  
2. Bloom on：日志出现 FG compile 摘要（pass 数、RT 数）；无泄漏的每帧 create。  
3. 关 Bloom 后 RT 创建次数下降（或 alias 命中）。  
4. Deferred 健康日志清单仍全部满足（见 [`deferred-acceptance.md`](deferred-acceptance.md)）。  

## 6. 明确不做（防范围蠕变）

- 整帧 Shadow→UI 进图  
- 自动推导 Pass 顺序（顺序仍由注册顺序 / 显式边声明）  
- TAA history / SSR（需要跨帧 import，属长期）  
- 替换 `PassExecContext` 借用指针模型  
- Editor 暴露「FrameGraph 编辑器」UI  

## 7. 何时从「短期」升级到本 cutsheet 实现

满足 **任意两条** 再开 FG 代码：

1. 已存在 ≥2 个独立 fullscreen 效果 Pass（不含 Final）。  
2. 第三次手写 half-res FBO + ping-pong。  
3. 画质选项需要「关效果即不分配 RT」。  

在此之前：只维护本文档，代码走 [`short-term-plan.md`](short-term-plan.md)。
