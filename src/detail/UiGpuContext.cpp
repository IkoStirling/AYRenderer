#include "detail/UiGpuContext.h"



#include "detail/BGFXAdapter.h"

#include "detail/GpuResources.h"



#include "AYShaderResourcePool.h"



#include <AYCoreUtility.h>



#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstring>



namespace ayt::render::detail {



namespace {



const char* kVaryingDef = R"(

vec4 v_color0    : COLOR0    = vec4(1.0, 0.0, 0.0, 1.0);

vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);



vec3 a_position  : POSITION;

vec4 a_color0    : COLOR0;

vec2 a_texcoord0 : TEXCOORD0;

)";



const char* kVsUi = R"(

$input a_position, a_color0, a_texcoord0

$output v_color0, v_texcoord0



#include <bgfx_shader.sh>



void main()

{

    gl_Position = vec4(a_position, 1.0);

    v_color0 = a_color0;

    v_texcoord0 = a_texcoord0;

}

)";



const char* kFsUi = R"(

$input v_color0, v_texcoord0



#include <bgfx_shader.sh>



SAMPLER2D(s_texColor, 0);



void main()

{

    vec4 tex = texture2D(s_texColor, v_texcoord0);

    gl_FragColor = vec4(v_color0.rgb * tex.rgb, v_color0.a * tex.a);

}

)";



bgfx::VertexLayout uiVertexLayout()

{

    bgfx::VertexLayout layout;

    layout.begin()

        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)

        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)

        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)

        .add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Float)

        .add(bgfx::Attrib::TexCoord2, 4, bgfx::AttribType::Float)

        .end();

    return layout;

}



void submitMesh(uint8_t viewId, BGFXAdapter& adapter, shader::ShaderResource& shader,

                shader::BindingId texBinding, uint16_t textureIdx, uint64_t state,

                const void* vertices, uint32_t vertexCount, uint32_t vertexStride,

                const uint32_t* indices, uint32_t indexCount)

{

    if (vertexCount == 0 || indexCount == 0 || !shader.isValid()) {

        return;

    }



    const bgfx::VertexLayout layout = uiVertexLayout();

    constexpr bool kIndex32         = true;
    const uint32_t availVertices = bgfx::getAvailTransientVertexBuffer(vertexCount, layout);

    const uint32_t availIndices  = bgfx::getAvailTransientIndexBuffer(indexCount, kIndex32);



    if (availVertices >= vertexCount && availIndices >= indexCount) {

        bgfx::TransientVertexBuffer tvb;

        bgfx::TransientIndexBuffer  tib;

        bgfx::allocTransientVertexBuffer(&tvb, vertexCount, layout);

        bgfx::allocTransientIndexBuffer(&tib, indexCount, kIndex32);

        const uint32_t dstStride = layout.getStride();
        const auto*    srcBytes  = static_cast<const uint8_t*>(vertices);
        auto*          dstBytes  = static_cast<uint8_t*>(tvb.data);
        if (dstStride == vertexStride) {
            std::memcpy(dstBytes, srcBytes, static_cast<size_t>(vertexCount) * vertexStride);
        } else {
            for (uint32_t i = 0; i < vertexCount; ++i) {
                std::memcpy(dstBytes + static_cast<size_t>(i) * dstStride,
                            srcBytes + static_cast<size_t>(i) * vertexStride, vertexStride);
            }
        }

        std::memcpy(tib.data, indices, static_cast<size_t>(indexCount) * sizeof(uint32_t));

        bgfx::setVertexBuffer(0, &tvb);

        bgfx::setIndexBuffer(&tib);

    } else {

        const bgfx::Memory* vbMem = bgfx::copy(vertices, vertexCount * vertexStride);

        const bgfx::Memory* ibMem =

            bgfx::copy(indices, static_cast<uint32_t>(indexCount * sizeof(uint32_t)));



        const bgfx::VertexBufferHandle vb = bgfx::createVertexBuffer(vbMem, layout);

        const bgfx::IndexBufferHandle  ib = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);



        if (!bgfx::isValid(vb) || !bgfx::isValid(ib)) {

            if (bgfx::isValid(vb)) {

                bgfx::destroy(vb);

            }

            if (bgfx::isValid(ib)) {

                bgfx::destroy(ib);

            }

            return;

        }



        adapter.setVertexBuffer(vb);

        adapter.setIndexBuffer(ib);



        shader.setTexture(0, texBinding, toShaderTexture(bgfx::TextureHandle{textureIdx}));



        shader::DrawCallContext ctx;

        ctx.viewId = viewId;

        ctx.state  = state;

        shader.submit(ctx);



        bgfx::destroy(vb);

        bgfx::destroy(ib);

        return;

    }



    shader.setTexture(0, texBinding, toShaderTexture(bgfx::TextureHandle{textureIdx}));



    shader::DrawCallContext ctx;

    ctx.viewId = viewId;

    ctx.state  = state;

    shader.submit(ctx);

}



// P2 — SDF rounded-rect shader pair. Vertices carry pixel-space position
// of the *submitted draw quad* in a_texcoord0 (may be expanded relative
// to the content silhouette for shadows / outside strokes) and the
// shape center + half-extent in a_texcoord1. Batch knife: u_rect is gone
// — identical SDF params merge into one submission while position/fill
// still vary per vertex. a_texcoord2 carries the soft-clip rect as
// (center, half-extent); all-zero = no clip pushed.
const char* kVaryingDefUiSdf = R"(
vec4 v_color0    : COLOR0    = vec4(1.0, 0.0, 0.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec2 v_pos       : TEXCOORD1 = vec2(0.0, 0.0);
vec4 v_shape     : TEXCOORD2 = vec4(0.0, 0.0, 0.0, 0.0);
vec4 v_clip      : TEXCOORD3 = vec4(0.0, 0.0, 0.0, 0.0);

vec3 a_position  : POSITION;
vec4 a_color0    : COLOR0;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_texcoord1 : TEXCOORD1;
vec4 a_texcoord2 : TEXCOORD2;
)";

const char* kVsUiSdf = R"(
$input a_position, a_color0, a_texcoord0, a_texcoord1, a_texcoord2
$output v_color0, v_pos, v_shape, v_clip

#include <bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position, 1.0);
    v_color0 = a_color0;
    // Pixel position across the submitted quad (NOT NDC→unit remap —
    // a_position is already screen NDC of an arbitrary UI rect).
    v_pos = a_texcoord0;
    // Shape center + half-extent, constant across the quad.
    v_shape = a_texcoord1;
    // Soft-clip rect (center, half-extent), constant across the quad.
    v_clip = a_texcoord2;
}
)";

const char* kFsUiSdf = R"(
$input v_color0, v_pos, v_shape, v_clip

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_radius;
uniform vec4 u_stroke;
uniform vec4 u_strokeWidth;
uniform vec4 u_shadow;
uniform vec4 u_shadowOffset;

// SDF of a rounded rect centered at the origin, half-extent b, per-corner
// radius r (TL TR BR BL). Radius picked by quadrant: p=(-,-)→r.x,
// p=(+,-)→r.y, p=(+,+)→r.z, p=(-,+)→r.w.
float sdRoundRect(vec2 p, vec2 b, vec4 r)
{
    float rSel = mix(
        mix(r.z, r.w, step(p.y, 0.0)),
        mix(r.y, r.x, step(p.y, 0.0)),
        step(p.x, 0.0));
    vec2 q = abs(p) - b + rSel;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rSel;
}

// Anti-aliased coverage. Prefer a *fixed* pixel width for strokes —
// fwidth(d) balloons at curved corners (∂d/∂x and ∂d/∂y both nonzero),
// so the ring thins/fades relative to straight edges.
float cover(float d)
{
    return clamp(0.5 - d / fwidth(d), 0.0, 1.0);
}

float coverPx(float d, float aaPx)
{
    return clamp(0.5 - d / max(aaPx, 1e-4), 0.0, 1.0);
}

void main()
{
    // Shape geometry arrives per-vertex (a_texcoord1) instead of a
    // uniform, so same-param items batch into one submission.
    vec2 center = v_shape.xy;
    vec2 halfB  = v_shape.zw;
    vec2 p      = v_pos - center;

    vec4 col = vec4(0.0, 0.0, 0.0, 0.0);

    // Shadow first (behind fill): shape = v_shape, shifted by offset.
    // Soft falloff width = blur (u_shadowOffset.z), not 1px fwidth —
    // otherwise blur 4 vs 12 look identical solid blobs.
    if (u_shadow.a > 0.0) {
        float blur = max(u_shadowOffset.z, 0.001);
        float d = sdRoundRect(p - u_shadowOffset.xy, halfB, u_radius);
        float cov = clamp(0.5 - d / blur, 0.0, 1.0);
        col.rgb += u_shadow.rgb * cov;
        col.a   += u_shadow.a   * cov;
    }

    // Fill from the per-vertex color.
    {
        float cov = cover(sdRoundRect(p, halfB, u_radius));
        col.rgb += v_color0.rgb * cov;
        col.a   += v_color0.a   * cov;
    }

    // Stroke of width w as a ring of the Euclidean offset curve — constant
    // pixel width on straight edges AND corners (unlike expanding the rect
    // while keeping the same radius, which thins the corners). Ring center
    // offset c = u_strokeWidth.y positions the band d ∈ [c-w/2, c+w/2]:
    // +w/2 = Outside (band [0, w], flush outside the edge), 0 = Center
    // (straddles the edge), -w/2 = Inside. drawCard maps BorderStyle::
    // Position to the inset; scalar drawBorderRect keeps Center — the
    // legacy visual.
    if (u_stroke.a > 0.0) {
        float d = sdRoundRect(p, halfB, u_radius);
        float w = u_strokeWidth.x;
        float c = u_strokeWidth.y;
        float ring = coverPx(abs(d - c) - w * 0.5, 1.0);
        col.rgb += u_stroke.rgb * ring;
        col.a   += u_stroke.a   * ring;
    }

    // Soft clip: fade coverage across the clip-rect edge (radius 0 =
    // plain box SDF). The draw quad is already CPU-intersected with the
    // clip, so this only re-adds AA where the quad edge cuts a stroke or
    // shadow gradient — scrolling an SDF card no longer hard-cuts its
    // AA ring at the clip boundary. All-zero clip (hw == 0) = no clip.
    float clipCover = 1.0;
    if (v_clip.z > 0.0 && v_clip.w > 0.0) {
        // (no scalar broadcast — vec4(0.0) is illegal HLSL, X3014)
        clipCover = cover(sdRoundRect(v_pos - v_clip.xy, v_clip.zw, vec4(0.0, 0.0, 0.0, 0.0)));
    }

    gl_FragColor = vec4(col.rgb * clipCover, min(col.a, 1.0) * clipCover);
}
)";



} // namespace



bool UiGpuContext::initialize(shader::ShaderResourcePool& shaderPool, BGFXAdapter& adapter)

{

    if (_initialized) {

        return true;

    }

    if (!adapter.isInitialized()) {

        std::fprintf(stderr, "[UiGpuContext] renderer adapter not ready\n");

        return false;

    }



    _shader = shaderPool.acquireFromBgfxSc(kVsUi, kFsUi, kVaryingDef, "editor_ui_flat");

    if (!_shader.isValid()) {

        const auto& errors = shaderPool.lastCompileErrors();

        std::fprintf(stderr, "[UiGpuContext] UI shader acquire failed\n");

        for (const std::string& err : errors) {

            std::fprintf(stderr, "  %s\n", err.c_str());

        }

        return false;

    }



    _texColorBinding = _shader.getTextureBinding("s_texColor");

    if (_texColorBinding == shader::InvalidBinding) {

        std::fprintf(stderr, "[UiGpuContext] s_texColor binding missing\n");

        shaderPool.release(_shader);

        return false;

    }



    const uint32_t whitePixel = 0xffffffffu;

    const bgfx::TextureHandle whiteHandle = bgfx::createTexture2D(

        1, 1, false, 1, bgfx::TextureFormat::BGRA8,

        BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,

        bgfx::copy(&whitePixel, sizeof(whitePixel)));

    if (!bgfx::isValid(whiteHandle)) {

        std::fprintf(stderr, "[UiGpuContext] white texture creation failed\n");

        shaderPool.release(_shader);

        _shader.reset();

        return false;

    }



    _whiteTexture = whiteHandle.idx;

    // P2 — SDF program for rounded-rect borders/shadows. Same pool/cacheKey
    // pattern as the flat shader; any missing binding fails init so callers
    // (and tests) see initialize() == false instead of a silently wrong shader.
    _sdfShader = shaderPool.acquireFromBgfxSc(kVsUiSdf, kFsUiSdf, kVaryingDefUiSdf, "editor_ui_sdf_cw");
    if (!_sdfShader.isValid()) {
        const auto& errors = shaderPool.lastCompileErrors();
        std::fprintf(stderr, "[UiGpuContext] UI SDF shader acquire failed\n");
        for (const std::string& err : errors) {
            std::fprintf(stderr, "  %s\n", err.c_str());
        }
        adapter.destroy(bgfx::TextureHandle{_whiteTexture});
        shaderPool.release(_shader);
        _shader.reset();
        return false;
    }
    std::fprintf(stderr, "[UiGpuContext] UI SDF shader OK (editor_ui_sdf_cw = constant-width stroke)\n");

    _sdfTexBinding    = _sdfShader.getTextureBinding("s_texColor");
    _sdfRadiusBinding = _sdfShader.getUniformBinding("u_radius");
    _sdfStrokeBinding = _sdfShader.getUniformBinding("u_stroke");
    _sdfStrokeWidBind = _sdfShader.getUniformBinding("u_strokeWidth");
    _sdfShadowBinding = _sdfShader.getUniformBinding("u_shadow");
    _sdfShadowOffBind = _sdfShader.getUniformBinding("u_shadowOffset");
    if (_sdfTexBinding == shader::InvalidBinding
        || _sdfRadiusBinding == shader::InvalidBinding || _sdfStrokeBinding == shader::InvalidBinding
        || _sdfStrokeWidBind == shader::InvalidBinding || _sdfShadowBinding == shader::InvalidBinding
        || _sdfShadowOffBind == shader::InvalidBinding) {
        std::fprintf(stderr, "[UiGpuContext] SDF shader bindings missing\n");
        adapter.destroy(bgfx::TextureHandle{_whiteTexture});
        shaderPool.release(_sdfShader);
        shaderPool.release(_shader);
        _shader.reset();
        return false;
    }

    _initialized  = true;

    return true;

}



void UiGpuContext::shutdown(shader::ShaderResourcePool& shaderPool, BGFXAdapter& adapter)

{

    if (bgfx::getRendererType() != bgfx::RendererType::Count) {

        if (_whiteTexture != kInvalidIdx) {

            adapter.destroy(bgfx::TextureHandle{_whiteTexture});

        }

    }



    if (_shader.isValid()) {

        shaderPool.release(_shader);

    }

    if (_sdfShader.isValid()) {

        shaderPool.release(_sdfShader);

    }



    _whiteTexture      = kInvalidIdx;

    _texColorBinding   = shader::InvalidBinding;

    _sdfTexBinding     = shader::InvalidBinding;

    _sdfRadiusBinding  = shader::InvalidBinding;

    _sdfStrokeBinding  = shader::InvalidBinding;

    _sdfStrokeWidBind  = shader::InvalidBinding;

    _sdfShadowBinding  = shader::InvalidBinding;

    _sdfShadowOffBind  = shader::InvalidBinding;

    _initialized       = false;

}



void UiGpuContext::beginView(uint8_t viewId, uint16_t width, uint16_t height)

{

    if (width < 1 || height < 1) {

        return;

    }



    // Target default backbuffer so UI/popups are not stuck on a stale FBO.

    // UI view id must also stay > PostProcess (see UIRenderBackend::kViewId).

    bgfx::setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);

    bgfx::setViewRect(viewId, 0, 0, width, height);

    bgfx::setViewMode(viewId, bgfx::ViewMode::Sequential);

    bgfx::setViewClear(viewId, BGFX_CLEAR_NONE);

    bgfx::touch(viewId);

}



void UiGpuContext::submitColoredQuads(uint8_t viewId, BGFXAdapter& adapter, uint64_t state,

                                      const void* vertices, uint32_t vertexCount,

                                      uint32_t vertexStride, const uint32_t* indices,

                                      uint32_t indexCount)

{

    if (!_initialized) {

        return;

    }

    submitMesh(viewId, adapter, _shader, _texColorBinding, _whiteTexture, state, vertices,

               vertexCount, vertexStride, indices, indexCount);

}



void UiGpuContext::submitTexturedQuads(uint8_t viewId, BGFXAdapter& adapter, uint64_t state,

                                       uint16_t textureIdx, const void* vertices,

                                       uint32_t vertexCount, uint32_t vertexStride,

                                       const uint32_t* indices, uint32_t indexCount)

{

    if (!_initialized || textureIdx == kInvalidIdx) {

        return;

    }

    submitMesh(viewId, adapter, _shader, _texColorBinding, textureIdx, state, vertices,

               vertexCount, vertexStride, indices, indexCount);

}



// P2 — SDF quads; identical-param runs submit together (batch knife):
// uniforms are set once per run, quads ride one transient buffer. The
// shape rect itself is per-vertex, so position/fill still vary inside
// the run. Same transient/fallback double path as submitMesh; the
// fallback path builds and destroys VB/IB per call (landmine #11 —
// acceptable at UI scale).
void UiGpuContext::submitSdfQuads(uint8_t viewId, BGFXAdapter& adapter, uint64_t state,

                                  const void* vertices, uint32_t vertexCount,

                                  uint32_t vertexStride, const uint32_t* indices,

                                  uint32_t indexCount, const SdfParams& params)

{

    if (!_initialized || vertexCount == 0 || indexCount == 0 || !_sdfShader.isValid()) {

        return;

    }



    // Uniform upload — shared by both buffer paths. u_strokeWidth packs
    // (width, inset); the shadow blur rides in u_shadowOffset.z.
    const float strokeWid[4] = {params.strokeWidth, params.strokeInset, 0.0f, 0.0f};
    const float shadowOff[4] = {params.shadowOffset.x, params.shadowOffset.y,
                                params.shadowBlur, 0.0f};
    _sdfShader.setUniform(_sdfRadiusBinding, &params.radius, sizeof(params.radius));
    _sdfShader.setUniform(_sdfStrokeBinding, &params.strokeColor, sizeof(params.strokeColor));
    _sdfShader.setUniform(_sdfStrokeWidBind, strokeWid, sizeof(strokeWid));
    _sdfShader.setUniform(_sdfShadowBinding, &params.shadowColor, sizeof(params.shadowColor));
    _sdfShader.setUniform(_sdfShadowOffBind, shadowOff, sizeof(shadowOff));



    const bgfx::VertexLayout layout = uiVertexLayout();

    constexpr bool kIndex32         = true;
    const uint32_t availVertices = bgfx::getAvailTransientVertexBuffer(vertexCount, layout);

    const uint32_t availIndices  = bgfx::getAvailTransientIndexBuffer(indexCount, kIndex32);



    if (availVertices >= vertexCount && availIndices >= indexCount) {

        bgfx::TransientVertexBuffer tvb;

        bgfx::TransientIndexBuffer  tib;

        bgfx::allocTransientVertexBuffer(&tvb, vertexCount, layout);

        bgfx::allocTransientIndexBuffer(&tib, indexCount, kIndex32);

        const uint32_t dstStride = layout.getStride();
        const auto*    srcBytes  = static_cast<const uint8_t*>(vertices);
        auto*          dstBytes  = static_cast<uint8_t*>(tvb.data);
        if (dstStride == vertexStride) {
            std::memcpy(dstBytes, srcBytes, static_cast<size_t>(vertexCount) * vertexStride);
        } else {
            for (uint32_t i = 0; i < vertexCount; ++i) {
                std::memcpy(dstBytes + static_cast<size_t>(i) * dstStride,
                            srcBytes + static_cast<size_t>(i) * vertexStride, vertexStride);
            }
        }

        std::memcpy(tib.data, indices, static_cast<size_t>(indexCount) * sizeof(uint32_t));

        bgfx::setVertexBuffer(0, &tvb);

        bgfx::setIndexBuffer(&tib);

        _sdfShader.setTexture(0, _sdfTexBinding, toShaderTexture(bgfx::TextureHandle{_whiteTexture}));

        shader::DrawCallContext ctx;

        ctx.viewId = viewId;

        ctx.state  = state;

        _sdfShader.submit(ctx);

    } else {

        const bgfx::Memory* vbMem = bgfx::copy(vertices, vertexCount * vertexStride);

        const bgfx::Memory* ibMem =

            bgfx::copy(indices, static_cast<uint32_t>(indexCount * sizeof(uint32_t)));



        const bgfx::VertexBufferHandle vb = bgfx::createVertexBuffer(vbMem, layout);

        const bgfx::IndexBufferHandle  ib = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);



        if (!bgfx::isValid(vb) || !bgfx::isValid(ib)) {

            if (bgfx::isValid(vb)) {

                bgfx::destroy(vb);

            }

            if (bgfx::isValid(ib)) {

                bgfx::destroy(ib);

            }

            return;

        }



        adapter.setVertexBuffer(vb);

        adapter.setIndexBuffer(ib);

        _sdfShader.setTexture(0, _sdfTexBinding, toShaderTexture(bgfx::TextureHandle{_whiteTexture}));

        shader::DrawCallContext ctx;

        ctx.viewId = viewId;

        ctx.state  = state;

        _sdfShader.submit(ctx);



        bgfx::destroy(vb);

        bgfx::destroy(ib);

    }

}



uint16_t UiGpuContext::uploadTextTexture(BGFXAdapter& adapter, uint16_t width, uint16_t height,

                                         const void* bgraPixels, uint32_t byteSize)

{

    AYUNREFERENCED_PARAM(adapter);

    if (bgraPixels == nullptr || byteSize == 0 || width == 0 || height == 0) {

        return kInvalidIdx;

    }



    const bgfx::TextureHandle handle = bgfx::createTexture2D(

        width, height, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_NONE,

        bgfx::copy(bgraPixels, byteSize));

    if (!bgfx::isValid(handle)) {

        return kInvalidIdx;

    }

    return handle.idx;

}



uint16_t UiGpuContext::uploadUiTexture(BGFXAdapter& adapter, uint16_t width, uint16_t height,
                                       const void* bgraPixels, uint32_t byteSize)
{
    AYUNREFERENCED_PARAM(adapter);

    if (bgraPixels == nullptr || byteSize == 0 || width == 0 || height == 0) {
        return kInvalidIdx;
    }

    // BGRA input, BGRA8 storage: bgfx interprets the byte order natively, so
    // pass pixels through unchanged (a byte-swap here would be a display-level
    // no-op). Clamp wrap: stretched art must not bleed edge texels across
    // seams (9-patch corners stay clean).
    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        width, height, false, 1, bgfx::TextureFormat::BGRA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(bgraPixels, byteSize));

    if (!bgfx::isValid(handle)) {
        return kInvalidIdx;
    }

    return handle.idx;
}



void UiGpuContext::releaseTextTexture(BGFXAdapter& adapter, uint16_t textureIdx)

{

    if (textureIdx == kInvalidIdx || textureIdx == _whiteTexture) {

        return;

    }

    adapter.destroy(bgfx::TextureHandle{textureIdx});

}



} // namespace ayt::render::detail

