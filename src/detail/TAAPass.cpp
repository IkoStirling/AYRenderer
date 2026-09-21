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
    texture2d sceneDepth
    uniform vec4 taaMetrics
    uniform vec4 taaParams
    uniform vec4 taaJitter
    uniform vec4 taaDepthParams
    uniform mat4 currentViewProjection
    vertex {
        in pos : position
        in uv : texcoord
        out vUv : texcoord = uv
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        // Output/history use the fixed grid. Only raw scene inputs use jitter.
        let outputUv = vUv
        let texel = taaMetrics.xy
        let uv = vec2(clamp(outputUv.x + taaJitter.x, texel.x * 0.5, 1.0 - texel.x * 0.5),
                      clamp(outputUv.y + taaJitter.y, texel.y * 0.5, 1.0 - texel.y * 0.5))
        let current = sample(currentColor, uv)
        // Bounds come from actual raw samples. Bilinear-filtered bounds can
        // shrink as jitter changes and repeatedly clip valid thin-edge history.
        let rawUv = (vec2(floor(uv.x * taaMetrics.z), floor(uv.y * taaMetrics.w))
                     + vec2(0.5, 0.5)) * texel

        // 3x3 current-frame neighborhood converted to YCoCg inline. Phoskia
        // intentionally has no user functions yet, so production Passes keep
        // all behavior visible to its type checker and disk cache.
        let uv0 = vec2(clamp(rawUv.x - texel.x, 0.0, 1.0), clamp(rawUv.y - texel.y, 0.0, 1.0))
        let uv1 = vec2(clamp(rawUv.x,           0.0, 1.0), clamp(rawUv.y - texel.y, 0.0, 1.0))
        let uv2 = vec2(clamp(rawUv.x + texel.x, 0.0, 1.0), clamp(rawUv.y - texel.y, 0.0, 1.0))
        let uv3 = vec2(clamp(rawUv.x - texel.x, 0.0, 1.0), clamp(rawUv.y,           0.0, 1.0))
        let uv5 = vec2(clamp(rawUv.x + texel.x, 0.0, 1.0), clamp(rawUv.y,           0.0, 1.0))
        let uv6 = vec2(clamp(rawUv.x - texel.x, 0.0, 1.0), clamp(rawUv.y + texel.y, 0.0, 1.0))
        let uv7 = vec2(clamp(rawUv.x,           0.0, 1.0), clamp(rawUv.y + texel.y, 0.0, 1.0))
        let uv8 = vec2(clamp(rawUv.x + texel.x, 0.0, 1.0), clamp(rawUv.y + texel.y, 0.0, 1.0))
        let c0 = sample(currentColor, uv0).xyz
        let c1 = sample(currentColor, uv1).xyz
        let c2 = sample(currentColor, uv2).xyz
        let c3 = sample(currentColor, uv3).xyz
        let c4 = sample(currentColor, rawUv).xyz
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
        let expansion = (neighborhoodMax - neighborhoodMin) * taaParams.z
                      + vec3(taaParams.z * 0.01, taaParams.z * 0.01, taaParams.z * 0.01)
        neighborhoodMin = neighborhoodMin - expansion
        neighborhoodMax = neighborhoodMax + expansion

        // Current RGB represents a footprint, not the center surface alone.
        // Dilate the nearest surface over 3x3 (standard non-reversed depth).
        // Read ALL temporal attributes from the chosen point, never average
        // foreground/background velocity or classify from a different point.
        let surfaceUv = uv
        let selectedDepth = sample(sceneDepth, uv).x
        let candidateDepth0 = sample(sceneDepth, uv0).x
        if (candidateDepth0 < selectedDepth) {
            selectedDepth = candidateDepth0
            surfaceUv = uv0
        }
        let candidateDepth1 = sample(sceneDepth, uv1).x
        if (candidateDepth1 < selectedDepth) {
            selectedDepth = candidateDepth1
            surfaceUv = uv1
        }
        let candidateDepth2 = sample(sceneDepth, uv2).x
        if (candidateDepth2 < selectedDepth) {
            selectedDepth = candidateDepth2
            surfaceUv = uv2
        }
        let candidateDepth3 = sample(sceneDepth, uv3).x
        if (candidateDepth3 < selectedDepth) {
            selectedDepth = candidateDepth3
            surfaceUv = uv3
        }
        let candidateDepth5 = sample(sceneDepth, uv5).x
        if (candidateDepth5 < selectedDepth) {
            selectedDepth = candidateDepth5
            surfaceUv = uv5
        }
        let candidateDepth6 = sample(sceneDepth, uv6).x
        if (candidateDepth6 < selectedDepth) {
            selectedDepth = candidateDepth6
            surfaceUv = uv6
        }
        let candidateDepth7 = sample(sceneDepth, uv7).x
        if (candidateDepth7 < selectedDepth) {
            selectedDepth = candidateDepth7
            surfaceUv = uv7
        }
        let candidateDepth8 = sample(sceneDepth, uv8).x
        if (candidateDepth8 < selectedDepth) {
            selectedDepth = candidateDepth8
            surfaceUv = uv8
        }
        let coverage = sample(geometryData, surfaceUv).w
        let validHistory = taaParams.w > 0.5
        let previousUv = outputUv
        // History alpha is private to TAA. Geometry stores its projected
        // depth; background stores -1. This rejects stale history at
        // disocclusions and prevents sky/geometry samples crossing edges.
        let currentHistoryDepth = -1.0
        let expectedPreviousDepth = -1.0
        if (coverage > 0.5) {
            let world = sample(worldPosition, surfaceUv).xyz
            let currentClip = currentViewProjection * vec4(world, 1.0)
            validHistory = validHistory && currentClip.w > 0.00001
            currentHistoryDepth = currentClip.z / max(currentClip.w, 0.00001)
            // Missing velocity is not zero velocity. In particular 2D and
            // failed/new draws must never consume an unrelated surface's history.
            validHistory = validHistory && taaJitter.w > 0.5
            if (taaJitter.w > 0.5) {
                let motion = sample(motionVectors, surfaceUv)
                validHistory = validHistory && motion.w > 0.5
                previousUv = outputUv - motion.xy
                expectedPreviousDepth = motion.z
            }
        } else {
            validHistory = validHistory && taaJitter.z > 0.5
        }
        validHistory = validHistory
            && previousUv.x >= 0.0 && previousUv.x <= 1.0
            && previousUv.y >= 0.0 && previousUv.y <= 1.0
        let safePreviousUv = vec2(clamp(previousUv.x, 0.0, 1.0),
                                  clamp(previousUv.y, 0.0, 1.0))
        // Bilinear RGB reconstruction with per-tap surface validation.
        // Sample texel centers: never interpolate depth or the -1 sky marker.
        let historyPixel = safePreviousUv * taaMetrics.zw - vec2(0.5, 0.5)
        let historyBase = vec2(floor(historyPixel.x), floor(historyPixel.y))
        let historyFraction = historyPixel - historyBase
        let historyBaseUv = (historyBase + vec2(0.5, 0.5)) * texel
        let h0 = sample(historyColor, historyBaseUv)
        let h1 = sample(historyColor, historyBaseUv + vec2(texel.x, 0.0))
        let h2 = sample(historyColor, historyBaseUv + vec2(0.0, texel.y))
        let h3 = sample(historyColor, historyBaseUv + texel)
        let w0 = (1.0 - historyFraction.x) * (1.0 - historyFraction.y)
        let w1 = historyFraction.x * (1.0 - historyFraction.y)
        let w2 = (1.0 - historyFraction.x) * historyFraction.y
        let w3 = historyFraction.x * historyFraction.y
        if (coverage > 0.5) {
            if (h0.w < 0.0 || abs(h0.w - expectedPreviousDepth) > taaDepthParams.x) { w0 = 0.0 }
            if (h1.w < 0.0 || abs(h1.w - expectedPreviousDepth) > taaDepthParams.x) { w1 = 0.0 }
            if (h2.w < 0.0 || abs(h2.w - expectedPreviousDepth) > taaDepthParams.x) { w2 = 0.0 }
            if (h3.w < 0.0 || abs(h3.w - expectedPreviousDepth) > taaDepthParams.x) { w3 = 0.0 }
        } else {
            if (h0.w >= 0.0) { w0 = 0.0 }
            if (h1.w >= 0.0) { w1 = 0.0 }
            if (h2.w >= 0.0) { w2 = 0.0 }
            if (h3.w >= 0.0) { w3 = 0.0 }
        }
        let historyWeight = w0 + w1 + w2 + w3
        validHistory = validHistory && historyWeight > 0.00001
        let historyRgb = (h0.xyz * w0 + h1.xyz * w1 + h2.xyz * w2 + h3.xyz * w3)
                         / max(historyWeight, 0.00001)
        let outputColor = vec4(current.xyz, currentHistoryDepth)
        let effectiveFeedback = 0.0
        let clipDifference = 0.0
        if (validHistory) {
            let history = vec3(
                dot(historyRgb, vec3(0.25, 0.50, 0.25)),
                dot(historyRgb, vec3(0.50, 0.00, -0.50)),
                dot(historyRgb, vec3(-0.25, 0.50, -0.25)))
            let unclippedHistory = history
            history = vec3(clamp(history.x, neighborhoodMin.x, neighborhoodMax.x),
                           clamp(history.y, neighborhoodMin.y, neighborhoodMax.y),
                           clamp(history.z, neighborhoodMin.z, neighborhoodMax.z))
            // In-neighborhood chroma/brightness changes can be pure coverage
            // changes under jitter. React only to history outside the box.
            let historyDifference = vec3(abs(unclippedHistory.x - history.x),
                                         abs(unclippedHistory.y - history.y),
                                         abs(unclippedHistory.z - history.z))
            clipDifference = length(historyDifference)
            let motionPixels = (previousUv - outputUv) * taaMetrics.zw
            let motionAmount = clamp(length(motionPixels) / 16.0, 0.0, 1.0)
            let feedback = mix(taaParams.x, taaParams.y, motionAmount)
            let unexpectedMismatch = historyDifference.x
            let luminanceMismatch = clamp(unexpectedMismatch * 4.0, 0.0, 1.0)
            let chromaMismatch = clamp(length(historyDifference.yz) * 2.0,
                                       0.0, 1.0)
            let reactiveMismatch = max(luminanceMismatch * 0.85,
                                       chromaMismatch * 0.65)
            feedback = feedback * (1.0 - reactiveMismatch)
            effectiveFeedback = clamp(feedback, 0.0, 0.95)
            let currentYCoCg = vec3(dot(current.xyz, vec3(0.25, 0.50, 0.25)),
                                    dot(current.xyz, vec3(0.50, 0.00, -0.50)),
                                    dot(current.xyz, vec3(-0.25, 0.50, -0.25)))
            let resolved = mix(currentYCoCg, history, effectiveFeedback)
            // Explicit grouping: Phoskia currently parses a-b-c as a-(b-c).
            // Losing the second minus destroys saturated blue history while
            // grayscale tests remain unchanged (Co=Cg=0).
            let resolvedRgb = vec3((resolved.x + resolved.y) - resolved.z,
                                   resolved.x + resolved.z,
                                   (resolved.x - resolved.y) - resolved.z)
            outputColor = vec4(clamp(resolvedRgb.x, 0.0, 1.0),
                               clamp(resolvedRgb.y, 0.0, 1.0),
                               clamp(resolvedRgb.z, 0.0, 1.0),
                               currentHistoryDepth)
        }
        // Optional second draw only; production history ALWAYS uses mode 0.
        // These outputs must never feed next frame's history.
        let debugMode = taaDepthParams.y
        if (debugMode > 0.5 && debugMode < 1.5) {
            outputColor = vec4(1.0, 0.0, 0.0, 1.0)
            if (validHistory) { outputColor = vec4(0.0, 1.0, 0.0, 1.0) }
        }
        if (debugMode > 1.5 && debugMode < 2.5) {
            outputColor = vec4(effectiveFeedback, effectiveFeedback, effectiveFeedback, 1.0)
        }
        if (debugMode > 2.5 && debugMode < 3.5) {
            let difference = clamp(clipDifference * 8.0, 0.0, 1.0)
            outputColor = vec4(difference, difference, difference, 1.0)
        }
        if (debugMode > 3.5 && debugMode < 4.5) {
            let velocityPixels = (outputUv - previousUv) * taaMetrics.zw
            outputColor = vec4(clamp(velocityPixels.x / 16.0 + 0.5, 0.0, 1.0),
                               clamp(velocityPixels.y / 16.0 + 0.5, 0.0, 1.0), 0.5, 1.0)
        }
        if (debugMode > 4.5) { outputColor = vec4(historyRgb, 1.0) }
        return outputColor
    }
}
)";

constexpr const char* kTaaCacheKey =
    "taa_phoskia_dilated_surface_history_v8";

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

float taaJitterRampScale(uint32_t stableFrameCount) noexcept
{
    return std::min(static_cast<float>(stableFrameCount)
                        / static_cast<float>(TAAPass::kJitterRampFrameCount),
                    1.0f);
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
    const bool perspective = std::abs(projection(3, 3)) < 1.0e-5f
        && std::abs(projection(3, 2)) > 1.0e-5f;
    const int offsetColumn = perspective ? 2 : 3;
    jittered(0, offsetColumn) +=
        2.0f * jitterPixels.x / static_cast<float>(width);
    // UI/sample Y grows downward; projection NDC Y grows upward.
    jittered(1, offsetColumn) -=
        2.0f * jitterPixels.y / static_cast<float>(height);
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
        && _tSceneDepth != ayt::shader::InvalidBinding
        && _uTaaMetrics != ayt::shader::InvalidBinding
        && _uTaaParams != ayt::shader::InvalidBinding
        && _uTaaJitter != ayt::shader::InvalidBinding
        && _uTaaDepthParams != ayt::shader::InvalidBinding
        && _uCurrentViewProjection != ayt::shader::InvalidBinding;
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
    // A prepared frame without a completed resolve cannot remain temporal history.
    if (_preparedThisFrame) {
        invalidateHistory();
    }
    _producedThisFrame = false;
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
    const bool hadPreviousCamera = _hasPreviousCamera;
    const bool cameraStable = hadPreviousCamera
        && maxAbsProjectionDelta(_previousBaseProjection, projection)
               <= kStableProjectionThreshold
        && maxAbsRotationDelta(_previousBaseView, view)
               <= kStableRotationThreshold
        && (cameraPosition - _previousCameraPosition).length()
               <= kStableTranslationDistance;
    const bool cameraCut = detectCameraCut(view, projection, cameraPosition);
    _backgroundHistoryValid = cameraStable && !cameraCut;
    if (cameraCut) {
        invalidateHistory();
    }
    _stableFrameCount = _historyValid && !cameraCut
        ? std::min(_stableFrameCount + 1u, kJitterRampFrameCount)
        : 0u;
    _writeHistoryIndex = static_cast<uint8_t>(1u - _readHistoryIndex);
    // Rejected history still returns a reconstructed fixed-grid current color.
    // Navigation no longer needs to disable jitter to conceal raw-grid wobble.
    const bool useProjectionJitter = _historyValid;
    TaaJitter jitter = useProjectionJitter
        ? taaHaltonJitter(_jitterSampleIndex) : TaaJitter{};
    const float jitterRamp = taaJitterRampScale(_stableFrameCount);
    jitter.x *= jitterRamp;
    jitter.y *= jitterRamp;
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
    const bgfx::TextureHandle sceneDepth = ctx.gbufferPass->gbufferDepthRt();
    const bool motionAvailable = ctx.motionVectorPass != nullptr
        && ctx.motionVectorPass->producedThisFrame()
        && BGFXAdapter::isValid(ctx.motionVectorPass->velocityTexture());
    const bgfx::TextureHandle motionVectors = motionAvailable
        ? ctx.motionVectorPass->velocityTexture()
        : worldPosition;
    if (!BGFXAdapter::isValid(currentColor)
        || !BGFXAdapter::isValid(historyColor)
        || !BGFXAdapter::isValid(worldPosition)
        || !BGFXAdapter::isValid(geometryData)
        || !BGFXAdapter::isValid(sceneDepth)) {
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
    _program.setTexture(5, _tSceneDepth, toShaderTexture(sceneDepth));
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
        _currentJitter.x
            / static_cast<float>(ctx.viewportWidth),
        _currentJitter.y
            / static_cast<float>(ctx.viewportHeight),
        _backgroundHistoryValid ? 1.0f : 0.0f,
        motionAvailable ? 1.0f : 0.0f,
    };
    _program.setUniform(_uTaaJitter, jitter, sizeof(jitter));
    const float depthParams[4] = {
        kHistoryDepthTolerance, 0.0f, 0.0f, 0.0f,
    };
    _program.setUniform(_uTaaDepthParams,
                        depthParams, sizeof(depthParams));
    float currentVp[16];
    toBgfxColumnMajor(_currentViewProjection, currentVp);
    _program.setUniform(_uCurrentViewProjection,
                        currentVp, sizeof(currentVp));
    adapter.setStateDepthTestAlways();

    ayt::shader::DrawCallContext draw;
    draw.viewId = kTaaViewId;
    draw.state = 0;
    _program.submit(draw);

    if (_debugView != 0) {
        // Re-evaluate with the SAME old history/input bindings, to a late
        // backbuffer view. submit() discards textures and pending uniforms:
        // explicitly rebind every input, never rely on previous draw state.
        configureFullscreenPassView(adapter, kDebugViewId, BGFX_INVALID_HANDLE,
            ctx.viewportX, ctx.viewportY, ctx.viewportWidth, ctx.viewportHeight);
        _geometry.bind(adapter);
        _program.setTexture(0, _tCurrentColor, toShaderTexture(currentColor));
        _program.setTexture(1, _tHistoryColor, toShaderTexture(historyColor));
        _program.setTexture(2, _tWorldPosition, toShaderTexture(worldPosition));
        _program.setTexture(3, _tGeometryData, toShaderTexture(geometryData));
        _program.setTexture(4, _tMotionVectors, toShaderTexture(motionVectors));
        _program.setTexture(5, _tSceneDepth, toShaderTexture(sceneDepth));
        _program.setUniform(_uTaaMetrics, metrics, sizeof(metrics));
        _program.setUniform(_uTaaParams, params, sizeof(params));
        _program.setUniform(_uTaaJitter, jitter, sizeof(jitter));
        _program.setUniform(_uCurrentViewProjection, currentVp, sizeof(currentVp));
        const float diagnosticParams[4] = {
            kHistoryDepthTolerance, static_cast<float>(_debugView), 0.0f, 0.0f};
        _program.setUniform(_uTaaDepthParams, diagnosticParams, sizeof(diagnosticParams));
        adapter.setStateDepthTestAlways();
        draw.viewId = kDebugViewId;
        _program.submit(draw);
    }

    ctx.frameGraph->markProduced(FgResourceId::TaaColor);
    ctx.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,
                                        FgResourceId::TaaColor);
    _readHistoryIndex = _writeHistoryIndex;
    _historyValid = true;
    _producedThisFrame = true;
    _previousBaseView = _currentBaseView;
    _previousBaseProjection = _currentBaseProjection;
    _previousCameraPosition = _currentCameraPosition;
    _hasPreviousCamera = true;
    if (_currentJitter.x != 0.0f || _currentJitter.y != 0.0f) {
        _jitterSampleIndex = (_jitterSampleIndex + 1u) % kJitterSampleCount;
    } else {
        // Bootstrap/cut frames have no history and do not consume the sequence.
        _jitterSampleIndex = 0u;
    }
    _preparedThisFrame = false;

    if (!_firstDispatchLogged) {
        std::fprintf(stderr,
                     "[TAAPass] first dispatch view=%u size=%ux%u "
                     "history=RGBA16F fixedGrid=1 reprojection=UnjitteredMotionDepth\n",
                     static_cast<unsigned>(kTaaViewId),
                     static_cast<unsigned>(ctx.viewportWidth),
                     static_cast<unsigned>(ctx.viewportHeight));
        _firstDispatchLogged = true;
    }
    return _debugView != 0 ? 2u : 1u;
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
    const auto sceneDepth = texture("sceneDepth");
    const auto metrics = uniform("taaMetrics");
    const auto params = uniform("taaParams");
    const auto jitter = uniform("taaJitter");
    const auto depthParams = uniform("taaDepthParams");
    const auto currentVp = uniform("currentViewProjection");
    if (!program.isValid()
        || current == ayt::shader::InvalidBinding
        || history == ayt::shader::InvalidBinding
        || world == ayt::shader::InvalidBinding
        || geometry == ayt::shader::InvalidBinding
        || motion == ayt::shader::InvalidBinding
        || sceneDepth == ayt::shader::InvalidBinding
        || metrics == ayt::shader::InvalidBinding
        || params == ayt::shader::InvalidBinding
        || jitter == ayt::shader::InvalidBinding
        || depthParams == ayt::shader::InvalidBinding
        || currentVp == ayt::shader::InvalidBinding) {
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
    _tSceneDepth = sceneDepth;
    _uTaaMetrics = metrics;
    _uTaaParams = params;
    _uTaaJitter = jitter;
    _uTaaDepthParams = depthParams;
    _uCurrentViewProjection = currentVp;
    _programRetryFrames = 0;
}

void TAAPass::finishFrame(MotionVectorPass* motion) noexcept
{
    if (!_producedThisFrame) {
        invalidateHistory();
        if (motion != nullptr) {
            motion->invalidateHistory();
        }
    } else if (motion != nullptr && !motion->producedThisFrame()) {
        motion->invalidateHistory();
    }
}

void TAAPass::invalidateHistory() noexcept
{
    _historyValid = false;
    _producedThisFrame = false;
    _preparedThisFrame = false;
    _readHistoryIndex = 0;
    _writeHistoryIndex = 1;
    _jitterSampleIndex = 0;
    _stableFrameCount = 0;
    _hasPreviousCamera = false;
    _backgroundHistoryValid = false;
    _currentJitter = {};
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
    _tSceneDepth = ayt::shader::InvalidBinding;
    _uTaaMetrics = ayt::shader::InvalidBinding;
    _uTaaParams = ayt::shader::InvalidBinding;
    _uTaaJitter = ayt::shader::InvalidBinding;
    _uTaaDepthParams = ayt::shader::InvalidBinding;
    _uCurrentViewProjection = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
    _firstDispatchLogged = false;
}

} // namespace ayt::render::detail
