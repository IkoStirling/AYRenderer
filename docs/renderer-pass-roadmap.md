# AYRenderer Pass 路线（2026-07-22，B0 docs 重置）

> 与 [`pass-lessons-from-shadow.md`](pass-lessons-from-shadow.md) / [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) / [`shadow-pass.md`](shadow-pass.md) / [`deferred-pass.md`](deferred-pass.md) 配套。  
> 记录 Shadow 验收后 + Deferred B0 docs 后的优先级，避免按过期 `execution-plan.md` 开工。

## 管线归属（答「谁组合的」）

- **Pass 列表与执行**在 **AYRenderer**：`RenderPipelineDesc::makeDefault()` =  
  `Shadow → ForwardOpaque → Transparent → PostProcess → UI`（`makeForwardWithShadows()` 同义别名）。
- **AYEditor 不自组 Pass**，只调用 `renderer.configurePipeline(makeForwardWithShadows())` 选用产品默认管线，并设 PP 波纹等宿主 knobs。
- **Deferred opt-in 路径**（P5 / B6 ship 后）：`renderer.configurePipeline(makeDeferred())` = `Shadow → GBuffer → Lighting → PostProcess → UI`。**默认 Forward 不变**（§5.3 红线 #4）。
- 换槽位 / 关 Shadow / 切 path：宿主传自定义 `RenderPipelineDesc::path` + 显式 `RenderPassSlot::Shadow` 即可。

## 当前默认管线（B0 之后）

```
Shadow → ForwardOpaque → Transparent → PostProcess → UI
```

| Pass | 状态 | 说明 |
|------|------|------|
| **ForwardOpaque** | 可用 | 含阴影采样；Alpha 已 skip 给 Transparent |
| **Shadow** | 表现基本通过 | R8 深度 + PCF；Phoskia 默认，`.sc` 可 A/B |
| **Transparent** | **已补齐** | 见 [`transparent-pass.md`](transparent-pass.md)；Editor 半透明验收物 |
| **PostProcess** | **已做实（波纹）** | 见 [`post-process.md`](post-process.md)；scene-FBO closure ship |
| **UI** | 可用 | Game View 合成 |
| **GBuffer** | **B0 docs ship ── B1–B6 待开** | docs 已发;无代码; 详见 [`deferred-pass.md`](deferred-pass.md) + [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) |
| **Lighting** | **B0 docs ship ── B1–B6 待开** | 同上,共享 Shadow 借用句柄 |

## 优先级

1. ~~**PostProcess 做实**~~ — 见 [`post-process.md`](post-process.md)。  
2. ~~**Transparent 补齐**~~ — 见 [`transparent-pass.md`](transparent-pass.md)。  
3. ~~**Shadow 默认 enabled (E5) + P4.2 bias 精修 + §5.5 PR-F1' 退役 + P6.5 Pass 裸 bgfx:: 收口**~~ — 全部 ship。  
4. **Deferred 最小闭环** — 大工程，B0 docs ship，**B1 (RenderPath enum)** 是下一刀候选。  
   切片见 [`execution-plan.md`](execution-plan.md) §P5 / [`pass-lessons-from-deferred.md`](pass-lessons-from-deferred.md) §2 ── B1→B6 渐进切，**严守 §5.3 红线**。
   - 为什么不急：Shadow 集成成本比预算高 ── 7 文件 + 双 viewId + blit→resolve + 龙骨矩阵链；Deferred 触碰面更广（4-slot MRT + 多光数据 + 路径切换），先 B0 docs 立 cutsheet 再 B1。  
5. **Shadow 增强（可选）** — CSM / pack 深度 / PCSS（与 Deferred 正交，留 Round 2）。

## 纪律（抄自 Shadow lessons）

- bgfx 目标：灯光/标量类 uniform 用 **vec4 + `.x`**，禁止依赖 GLSL uniform 初值。  
- 先 hand / emit 对齐，再切默认；改 shader 必 bump cache key。  
- 默认管线变更严守 §5.3 ── 新 Pass 必须 opt-in，不变默认。  
- FrameContext / RenderScene 多光源数据**永不**进；走 `PassExecContext::*` 借用指针模式（`shadowPass` → `gbufferPass` 镜像）。  
- Pass 内禁止 `<bgfx/bgfx.h>` 直 include ── 公开头零 bgfx（P6.5 ship）。
