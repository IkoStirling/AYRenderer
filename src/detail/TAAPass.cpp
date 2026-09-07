#include "detail/TAAPass.h"

#include "detail/BgfxMatrix.h"
#include "detail/FgResource.h"
#include "detail/GBufferPass.h"
#include "detail/MotionVectorPass.h"
#include "detail/GpuResources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

constexpr const char* kTaaPhoskiaSource = R"(
material TemporalAA {
    texture2d currentColor
    texture2d historyColor
    texture2d worldPosition
    texture2d geometryData
    texture2d motionVectors
    uniform vec4 taaMetrics
    uniform vec4 taaParams
    uniform vec4 taaJitter
    uniform mat4 previousViewProjection
    vertex {
        in pos : position
        in uv : texcoord
        out vUv : texcoord = uv
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let uv = vec2(clamp(vUv.x, 0.0, 1.0),
                      clamp(vUv.y, 0.0, 1.0))
        let texel = taaMetrics.xy
        let current = sample(currentColor, uv)

        // 3x3 current-frame neighborhood converted to YCoCg inline. Phoskia
        // intentionally has no user functions yet, so production Passes keep
        // all behavior visible to its type checker and disk cache.
        let uv0 = vec2(clamp(uv.x - texel.x, 0.0, 1.0), clamp(uv.y - texel.y, 0.0, 1.0))
        let uv1 = vec2(clamp(uv.x,           0.0, 1.0), clamp(uv.y - texel.y, 0.0, 1.0))
        let uv2 = vec2(clamp(uv.x + texel.x, 0.0, 1.0), clamp(uv.y - texel.y, 0.0, 1.0))
        let uv3 = vec2(clamp(uv.x - texel.x, 0.0, 1.0), clamp(uv.y,           0.0, 1.0))
        let uv5 = vec2(clamp(uv.x + texel.x, 0.0, 1.0), clamp(uv.y,           0.0, 1.0))
        let uv6 = vec2(clamp(uv.x - texel.x, 0.0, 1.0), clamp(uv.y + texel.y, 0.0, 1.0))
        let uv7 = vec2(clamp(uv.x,           0.0, 1.0), clamp(uv.y + texel.y, 0.0, 1.0))
        let uv8 = vec2(clamp(uv.x + texel.x, 0.0, 1.0), clamp(uv.y + texel.y, 0.0, 1.0))
        let c0 = sample(currentColor, uv0).xyz
        let c1 = sample(currentColor, uv1).xyz
        let c2 = sample(currentColor, uv2).xyz
        let c3 = sample(currentColor, uv3).xyz
        let c4 = current.xyz
        let c5 = sample(currentColor, uv5).xyz
        let c6 = sample(currentColor, uv6).xyz
        let c7 = sample(currentColor, uv7).xyz
        let c8 = sample(currentColor, uv8).xyz
        let n0 = vec3(dot(c0, vec3(0.25, 0.50, 0.25)), dot(c0, vec3(0.50, 0.00, -0.50)), dot(c0, vec3(-0.25, 0.50, -0.25)))
        let n1 = vec3(dot(c1, vec3(0.25, 0.50, 0.25)), dot(c1, vec3(0.50, 0.00, -0.50)), dot(c1, vec3(-0.25, 0.50, -0.25)))
        let n2 = vec3(dot(c2, vec3(0.25, 0.50, 0.25)), dot(c2, vec3(0.50, 0.00, -0.50)), dot(c2, vec3(-0.25, 0.50, -0.25)))
        let n3 = vec3(dot(c3, vec3(0.25, 0.50, 0.25)), dot(c3, vec3(0.50, 0.00, -0.50)), dot(c3, vec3(-0.25, 0.50, -0.25)))
        let n4 = vec3(dot(c4, vec3(0.25, 0.50, 0.25)), dot(c4, vec3(0.50, 0.00, -0.50)), dot(c4, vec3(-0.25, 0.50, -0.25)))
        let n5 = vec3(dot(c5, vec3(0.25, 0.50, 0.25)), dot(c5, vec3(0.50, 0.00, -0.50)), dot(c5, vec3(-0.25, 0.50, -0.25)))
        let n6 = vec3(dot(c6, vec3(0.25, 0.50, 0.25)), dot(c6, vec3(0.50, 0.00, -0.50)), dot(c6, vec3(-0.25, 0.50, -0.25)))
        let n7 = vec3(dot(c7, vec3(0.25, 0.50, 0.25)), dot(c7, vec3(0.50, 0.00, -0.50)), dot(c7, vec3(-0.25, 0.50, -0.25)))
        let n8 = vec3(dot(c8, vec3(0.25, 0.50, 0.25)), dot(c8, vec3(0.50, 0.00, -0.50)), dot(c8, vec3(-0.25, 0.50, -0.25)))
        let neighborhoodMin = vec3(
            min(min(min(n0.x, n1.x), min(n2.x, n3.x)), min(min(n4.x, n5.x), min(n6.x, min(n7.x, n8.x)))),
            min(min(min(n0.y, n1.y), min(n2.y, n3.y)), min(min(n4.y, n5.y), min(n6.y, min(n7.y, n8.y)))),
            min(min(min(n0.z, n1.z), min(n2.z, n3.z)), min(min(n4.z, n5.z), min(n6.z, min(n7.z, n8.z)))))
        let neighborhoodMax = vec3(
            max(max(max(n0.x, n1.x), max(n2.x, n3.x)), max(max(n4.x, n5.x), max(n6.x, max(n7.x, n8.x)))),
            max(max(max(n0.y, n1.y), max(n2.y, n3.y)), max(max(n4.y, n5.y), max(n6.y, max(n7.y, n8.y)))),
            max(max(max(n0.z, n1.z), max(n2.z, n3.z)), max(max(n4.z, n5.z), max(n6.z, max(n7.z, n8.z)))))
        let localLumaSpan = max(neighborhoodMax.x - neighborhoodMin.x, 0.0)
        let expansion = (neighborhoodMax - neighborhoodMin) * taaParams.z
                      + vec3(taaParams.z * 0.01, taaParams.z * 0.01, taaParams.z * 0.01)
        neighborhoodMin = neighborhoodMin - expansion
        neighborhoodMax = neighborhoodMax + expansion

        let coverage = sample(geometryData, uv).w
        let validHistory = taaParams.w > 0.5
        let previousUv = uv + taaJitter.xy
        if (coverage > 0.5) {
            if (taaJitter.w > 0.5) {
                // Velocity convention: current UV minus previous UV.
                previousUv = uv - sample(motionVectors, uv).xy
            } else {
                let world = sample(worldPosition, uv).xyz
                let previousClip = previousViewProjection * vec4(world, 1.0)
                validHistory = validHistory && previousClip.w > 0.00001
                let previousNdc = previousClip.xy / max(previousClip.w, 0.00001)
                previousUv = vec2(previousNdc.x * 0.5 + 0.5,
                                  1.0 - (previousNdc.y * 0.5 + 0.5))
            }
        } else {
            validHistory = validHistory && taaJitter.z > 0.5
        }
        validHistory = validHistory
            && previousUv.x >= 0.0 && previousUv.x <= 1.0
            && previousUv.y >= 0.0 && previousUv.y <= 1.0
        let outputColor = current
        if (validHistory) {
            let historyRgb = sample(historyColor, previousUv).xyz
            let history = vec3(
                dot(historyRgb, vec3(0.25, 0.50, 0.25)),
                dot(historyRgb, vec3(0.50, 0.00, -0.50)),
                dot(historyRgb, vec3(-0.25, 0.50, -0.25)))
            history = vec3(clamp(history.x, neighborhoodMin.x, neighborhoodMax.x),
                           clamp(history.y, neighborhoodMin.y, neighborhoodMax.y),
                           clamp(history.z, neighborhoodMin.z, neighborhoodMax.z))
            let motionPixels = (previousUv - uv) * taaMetrics.zw
            let motionAmount = clamp(length(motionPixels) / 16.0, 0.0, 1.0)
            let feedback = mix(taaParams.x, taaParams.y, motionAmount)
            let unexpectedMismatch = max(abs(history.x - n4.x)
                                       - localLumaSpan * 0.5, 0.0)
            let luminanceMismatch = clamp(unexpectedMismatch * 4.0, 0.0, 1.0)
            feedback = feedback * (1.0 - luminanceMismatch * 0.75)
            let resolved = mix(n4, history, clamp(feedback, 0.0, 0.95))
            let resolvedRgb = vec3(resolved.x + resolved.y - resolved.z,
                                   resolved.x + resolved.z,
                                   resolved.x - resolved.y - resolved.z)
            outputColor = vec4(clamp(resolvedRgb.x, 0.0, 1.0),
                               clamp(resolvedRgb.y, 0.0, 1.0),
                               clamp(resolvedRgb.z, 0.0, 1.0), current.w)
        }
        return outputColor
    }
}
)";

constexpr const char* kTaaCacheKey = "taa_phoskia_motion_vectors_ycocg_v5";

float halton(uint32_t index, uint32_t base) noexcept
{
    float value = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        value += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return value;
}

float maxAbsProjectionDelta(const ayt::math::Float4x4& a,
                            const ayt::math::Float4x4& b) noexcept
{
    float delta = 0.0f;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            delta = std::max(delta, std::abs(a(row, col) - b(row, col)));
        }
    }
    return delta;
}

float maxAbsRotationDelta(const ayt::math::Float4x4& a,
                          const ayt::math::Float4x4& b) noexcept
{
    float delta = 0.0f;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            delta = std::max(delta, std::abs(a(row, col) - b(row, col)));
        }
    }
    return delta;
}

} // namespace

const char* const kTaaCacheKeyCStr = kTaaCacheKey;

const char* taaPhoskiaSourceForTests() noexcept { return kTaaPhoskiaSource; }

TaaJitter taaHaltonJitter(uint32_t sampleIndex) noexcept
{
    const uint32_t index = sampleIndex % TAAPass::kJitterSampleCount + 1u;
    return {(halton(index, 2u) - 0.5f) * TAAPass::kJitterSpread,
            (halton(index, 3u) - 0.5f) * TAAPass::kJitterSpread};
}

ayt::math::Float4x4 taaApplyProjectionJitter(
    const ayt::math::Float4x4& projection,
    TaaJitter jitterPixels,
    uint16_t width,
    uint16_t height) noexcept
{
    ayt::math::Float4x4 jittered = projection;
    if (width == 0 || height == 0) {
        return jittered;
    }
    jittered(0, 2) += 2.0f * jitterPixels.x / static_cast<float>(width);
    // UI/sample Y grows downward; projection NDC Y grows upward.
    jittered(1, 2) -= 2.0f * jitterPixels.y / static_cast<float>(height);
    return jittered;
}

bool TAAPass::isReady() const noexcept
{
    return _geometry.isReady() && _program.isValid()
        && BGFXAdapter::isValid(_history[0])
        && BGFXAdapter::isValid(_history[1])
        && _tCurrentColor != ayt::shader::InvalidBinding
        && _tHistoryColor != ayt::shader::InvalidBinding
        && _tWorldPosition != ayt::shader::InvalidBinding
        && _tGeometryData != ayt::shader::InvalidBinding
        && _tMotionVectors != ayt::shader::InvalidBinding
        && _uTaaMetrics != ayt::shader::InvalidBinding
        && _uTaaParams != ayt::shader::InvalidBinding
        && _uTaaJitter != ayt::shader::InvalidBinding
        && _uPreviousViewProjection != ayt::shader::InvalidBinding;
}

bool TAAPass::detectCameraCut(
    const ayt::math::Float4x4& view,
    const ayt::math::Float4x4& projection,
    const ayt::math::FVector3& cameraPosition) const noexcept
{
    if (!_hasPreviousCamera) {
        return false;
    }
    constexpr float kProjectionCutThreshold = 0.05f;
    constexpr float kRotationCutThreshold = 0.35f;
    constexpr float kTranslationCutDistance = 4.0f;
    return maxAbsProjectionDelta(_previousBaseProjection, projection)
               > kProjectionCutThreshold
        || maxAbsRotationDelta(_previousBaseView, view)
               > kRotationCutThreshold
        || (cameraPosition - _previousCameraPosition).length()
               > kTranslationCutDistance;
}

bool TAAPass::prepareFrame(BGFXAdapter& adapter,
                           ayt::shader::ShaderResourcePool& pool,
                           const ayt::math::Float4x4& view,
                           const ayt::math::Float4x4& projection,
                           const ayt::math::FVector3& cameraPosition,
                           uint16_t width,
                           uint16_t height)
{
    _preparedThisFrame = false;
    _jitteredProjection = projection;
    if (!isEnabled() || !adapter.isInitialized() || adapter.isNoopBackend()
        || width == 0 || height == 0) {
        return false;
    }
    if (!_geometry.ensure(adapter) || !ensureHistory(adapter, width, height)) {
        return false;
    }
    ensureProgram(pool);
    if (!isReady()) {
        return false;
    }

    constexpr float kStableProjectionThreshold = 1.0e-5f;
    constexpr float kStableRotationThreshold = 1.0e-5f;
    constexpr float kStableTranslationDistance = 1.0e-4f;
    _backgroundHistoryValid = _hasPreviousCamera
        && maxAbsProjectionDelta(_previousBaseProjection, projection)
               <= kStableProjectionThreshold
        && maxAbsRotationDelta(_previousBaseView, view)
               <= kStableRotationThreshold
        && (cameraPosition - _previousCameraPosition).length()
               <= kStableTranslationDistance;
    if (detectCameraCut(view, projection, cameraPosition)) {
        invalidateHistory();
    }
    _writeHistoryIndex = static_cast<uint8_t>(1u - _readHistoryIndex);
    const TaaJitter jitter = taaHaltonJitter(_jitterSampleIndex);
    _currentJitter = jitter;
    _jitteredProjection = taaApplyProjectionJitter(
        projection, jitter, width, height);
    _currentBaseView = view;
    _currentBaseProjection = projection;
    _currentCameraPosition = cameraPosition;
    _currentViewProjection = _jitteredProjection * view;
    _preparedThisFrame = true;
    return true;
}

bgfx::FrameBufferHandle TAAPass::writeHistoryFbo() const noexcept
{
    return _history[_writeHistoryIndex];
}

uint32_t TAAPass::execute(PassExecContext& ctx)
{
    if (!_preparedThisFrame || ctx.frameGraph == nullptr
        || ctx.gbufferPass == nullptr
        || !ctx.gbufferPass->producedThisFrame()
        || !ctx.frameGraph->semanticProducedThisFrame(FgSemantic::PresentSource)) {
        return 0;
    }

    BGFXAdapter& adapter = ctx.adapter;
    const bgfx::FrameBufferHandle source =
        ctx.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::TaaColor);
    if (!BGFXAdapter::isValid(source) || !BGFXAdapter::isValid(target)
        || target.idx != writeHistoryFbo().idx) {
        rateLimitedEarlyReturn("TAAPass", "FrameGraph source/history target invalid");
        return 0;
    }

    const bgfx::TextureHandle currentColor = adapter.getFboAttachment(source, 0);
    const bgfx::TextureHandle historyColor =
        adapter.getFboAttachment(_history[_readHistoryIndex], 0);
    const bgfx::TextureHandle worldPosition =
        ctx.gbufferPass->gbufferWorldPositionRt();
    const bgfx::TextureHandle geometryData =
        ctx.gbufferPass->gbufferMaterialRt();
    const bool motionAvailable = ctx.motionVectorPass != nullptr
        && ctx.motionVectorPass->producedThisFrame()
        && BGFXAdapter::isValid(ctx.motionVectorPass->velocityTexture());
    const bgfx::TextureHandle motionVectors = motionAvailable
        ? ctx.motionVectorPass->velocityTexture()
        : worldPosition;
    if (!BGFXAdapter::isValid(currentColor)
        || !BGFXAdapter::isValid(historyColor)
        || !BGFXAdapter::isValid(worldPosition)
        || !BGFXAdapter::isValid(geometryData)) {
        rateLimitedEarlyReturn("TAAPass", "current/history/GBuffer attachment invalid");
        return 0;
    }

    configureFullscreenPassView(adapter, kTaaViewId, target, 0, 0,
                                ctx.viewportWidth, ctx.viewportHeight);
    _geometry.bind(adapter);
    _program.setTexture(0, _tCurrentColor, toShaderTexture(currentColor));
    _program.setTexture(1, _tHistoryColor, toShaderTexture(historyColor));
    _program.setTexture(2, _tWorldPosition, toShaderTexture(worldPosition));
    _program.setTexture(3, _tGeometryData, toShaderTexture(geometryData));
    _program.setTexture(4, _tMotionVectors, toShaderTexture(motionVectors));
    const float metrics[4] = {
        1.0f / static_cast<float>(ctx.viewportWidth),
        1.0f / static_cast<float>(ctx.viewportHeight),
        static_cast<float>(ctx.viewportWidth),
        static_cast<float>(ctx.viewportHeight),
    };
    const float params[4] = {
        kStaticHistoryWeight,
        kMovingHistoryWeight,
        kNeighborhoodExpansion,
        _historyValid ? 1.0f : 0.0f,
    };
    _program.setUniform(_uTaaMetrics, metrics, sizeof(metrics));
    _program.setUniform(_uTaaParams, params, sizeof(params));
    const float jitter[4] = {
        (_previousJitter.x - _currentJitter.x)
            / static_cast<float>(ctx.viewportWidth),
        (_previousJitter.y - _currentJitter.y)
            / static_cast<float>(ctx.viewportHeight),
        _backgroundHistoryValid ? 1.0f : 0.0f,
        motionAvailable ? 1.0f : 0.0f,
    };
    _program.setUniform(_uTaaJitter, jitter, sizeof(jitter));
    float previousVp[16];
    toBgfxColumnMajor(_previousViewProjection, previousVp);
    _program.setUniform(_uPreviousViewProjection,
                        previousVp, sizeof(previousVp));
    adapter.setStateDepthTestAlways();

    ayt::shader::DrawCallContext draw;
    draw.viewId = kTaaViewId;
    draw.state = 0;
    _program.submit(draw);

    ctx.frameGraph->markProduced(FgResourceId::TaaColor);
    ctx.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,
                                        FgResourceId::TaaColor);
    _readHistoryIndex = _writeHistoryIndex;
    _historyValid = true;
    _previousViewProjection = _currentViewProjection;
    _previousBaseView = _currentBaseView;
    _previousBaseProjection = _currentBaseProjection;
    _previousCameraPosition = _currentCameraPosition;
    _previousJitter = _currentJitter;
    _hasPreviousCamera = true;
    _jitterSampleIndex = (_jitterSampleIndex + 1u) % kJitterSampleCount;
    _preparedThisFrame = false;

    if (!_firstDispatchLogged) {
        std::fprintf(stderr,
                     "[TAAPass] first dispatch view=%u size=%ux%u "
                     "history=RGBA16F reprojection=MotionVector/fallbackWorldPos\n",
                     static_cast<unsigned>(kTaaViewId),
                     static_cast<unsigned>(ctx.viewportWidth),
                     static_cast<unsigned>(ctx.viewportHeight));
        _firstDispatchLogged = true;
    }
    return 1;
}

bool TAAPass::ensureHistory(BGFXAdapter& adapter,
                            uint16_t width,
                            uint16_t height)
{
    if (_historyWidth == width && _historyHeight == height
        && BGFXAdapter::isValid(_history[0])
        && BGFXAdapter::isValid(_history[1])) {
        return true;
    }
    destroyHistory(adapter);
    _history[0] = adapter.createFrameBuffer(
        width, height, bgfx::TextureFormat::RGBA16F,
        /*withDepth=*/false, /*pointSampled=*/false);
    _history[1] = adapter.createFrameBuffer(
        width, height, bgfx::TextureFormat::RGBA16F,
        /*withDepth=*/false, /*pointSampled=*/false);
    if (!BGFXAdapter::isValid(_history[0])
        || !BGFXAdapter::isValid(_history[1])) {
        destroyHistory(adapter);
        return false;
    }
    _historyWidth = width;
    _historyHeight = height;
    invalidateHistory();
    return true;
}

void TAAPass::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    if (_program.isValid()) {
        return;
    }
    if (_programRetryFrames > 0) {
        --_programRetryFrames;
        return;
    }
    ayt::shader::ShaderResource program =
        pool.acquire(kTaaPhoskiaSource, kTaaCacheKey);
    auto texture = [&program](const char* name) {
        return program.isValid() ? program.getTextureBinding(name)
                                 : ayt::shader::InvalidBinding;
    };
    auto uniform = [&program](const char* name) {
        return program.isValid() ? program.getUniformBinding(name)
                                 : ayt::shader::InvalidBinding;
    };
    const auto current = texture("currentColor");
    const auto history = texture("historyColor");
    const auto world = texture("worldPosition");
    const auto geometry = texture("geometryData");
    const auto motion = texture("motionVectors");
    const auto metrics = uniform("taaMetrics");
    const auto params = uniform("taaParams");
    const auto jitter = uniform("taaJitter");
    const auto previous = uniform("previousViewProjection");
    if (!program.isValid()
        || current == ayt::shader::InvalidBinding
        || history == ayt::shader::InvalidBinding
        || world == ayt::shader::InvalidBinding
        || geometry == ayt::shader::InvalidBinding
        || motion == ayt::shader::InvalidBinding
        || metrics == ayt::shader::InvalidBinding
        || params == ayt::shader::InvalidBinding
        || jitter == ayt::shader::InvalidBinding
        || previous == ayt::shader::InvalidBinding) {
        constexpr uint16_t kRetryIntervalFrames = 120;
        _programRetryFrames = kRetryIntervalFrames;
        std::fprintf(stderr,
                     "[TAAPass] shader acquire/binding failed; retrying "
                     "in %u frames.\n",
                     static_cast<unsigned>(kRetryIntervalFrames));
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[TAAPass]   %s\n", error.c_str());
        }
        return;
    }
    _program = std::move(program);
    _tCurrentColor = current;
    _tHistoryColor = history;
    _tWorldPosition = world;
    _tGeometryData = geometry;
    _tMotionVectors = motion;
    _uTaaMetrics = metrics;
    _uTaaParams = params;
    _uTaaJitter = jitter;
    _uPreviousViewProjection = previous;
    _programRetryFrames = 0;
}

void TAAPass::invalidateHistory() noexcept
{
    _historyValid = false;
    _preparedThisFrame = false;
    _readHistoryIndex = 0;
    _writeHistoryIndex = 1;
    _jitterSampleIndex = 0;
    _hasPreviousCamera = false;
    _backgroundHistoryValid = false;
    _currentJitter = {};
    _previousJitter = {};
    _previousViewProjection = ayt::math::Float4x4::identity();
}

void TAAPass::destroyHistory(BGFXAdapter& adapter)
{
    for (bgfx::FrameBufferHandle& fbo : _history) {
        if (BGFXAdapter::isValid(fbo)) {
            adapter.destroy(fbo);
        }
        fbo = BGFX_INVALID_HANDLE;
    }
    _historyWidth = 0;
    _historyHeight = 0;
    invalidateHistory();
}

void TAAPass::destroyResources(BGFXAdapter& adapter)
{
    destroyHistory(adapter);
    _geometry.destroy(adapter);
    _program.reset();
    _tCurrentColor = ayt::shader::InvalidBinding;
    _tHistoryColor = ayt::shader::InvalidBinding;
    _tWorldPosition = ayt::shader::InvalidBinding;
    _tGeometryData = ayt::shader::InvalidBinding;
    _tMotionVectors = ayt::shader::InvalidBinding;
    _uTaaMetrics = ayt::shader::InvalidBinding;
    _uTaaParams = ayt::shader::InvalidBinding;
    _uTaaJitter = ayt::shader::InvalidBinding;
    _uPreviousViewProjection = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
    _firstDispatchLogged = false;
}

} // namespace ayt::render::detail
