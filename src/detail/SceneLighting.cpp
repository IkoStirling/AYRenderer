#include "detail/SceneLighting.h"

#include "detail/ShadowAtlas.h"
#include "detail/ShadowPass.h"

#include "AYShader/ShaderResource.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ayt::render::detail
{

namespace {

float finiteOr(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

void initializeInactiveSlots(PackedSceneLighting& packed) noexcept
{
    for (uint32_t i = 0; i < kMaxSceneLights; ++i) {
        // The shaders are deliberately branchless. Keep inactive slots finite
        // so point attenuation and spot smoothstep never manufacture NaNs.
        packed.dirs[i][1] = 1.0f;
        packed.params[i][0] = 1.0f;
        packed.params[i][1] = 0.0f;
        packed.params[i][2] = 1.0f;
        packed.params[i][3] = 0.0f;
        packed.spotDirs[i][1] = -1.0f;
    }
}

void initializeShadowMatrices(PackedShadowAtlas& packed) noexcept
{
    for (uint32_t i = 0; i < kMaxSceneLights; ++i) {
        packed.lightViewProjs[i][0] = 1.0f;
        packed.lightViewProjs[i][5] = 1.0f;
        packed.lightViewProjs[i][10] = 1.0f;
        packed.lightViewProjs[i][15] = 1.0f;
    }
}

bool uploadVec4Array(ayt::shader::ShaderResource& shader,
                     const char* name,
                     const float* values,
                     uint32_t byteCount)
{
    const ayt::shader::BindingId binding = shader.getUniformBinding(name);
    if (binding == ayt::shader::InvalidBinding) {
        return false;
    }
    shader.setUniform(binding, values, byteCount);
    return true;
}

} // namespace

PackedSceneLighting packSceneLighting(const SceneLights* sceneLights,
                                      const FrameContext& frame) noexcept
{
    PackedSceneLighting packed{};
    initializeInactiveSlots(packed);

    if (sceneLights == nullptr || sceneLights->count == 0u) {
        packed.activeLightCount = 1u;
        packed.dirs[0][0] = -finiteOr(frame.lightDirection.x, 0.0f);
        packed.dirs[0][1] = -finiteOr(frame.lightDirection.y, -1.0f);
        packed.dirs[0][2] = -finiteOr(frame.lightDirection.z, 0.0f);
        packed.colors[0][0] = std::max(finiteOr(frame.lightColor.x, 0.0f), 0.0f);
        packed.colors[0][1] = std::max(finiteOr(frame.lightColor.y, 0.0f), 0.0f);
        packed.colors[0][2] = std::max(finiteOr(frame.lightColor.z, 0.0f), 0.0f);
        return packed;
    }

    // ShadowPass uses the same stable partition: supported casters first,
    // followed by the unshadowed suffix. This makes light slot i and atlas
    // slot i refer to the same source light in every consumer.
    const ShadowLightOrder order = computeShadowLightOrder(*sceneLights);
    packed.activeLightCount = order.lightCount;

    for (uint32_t i = 0; i < order.lightCount; ++i) {
        const Light& light = sceneLights->lights[order.indices[i]];

        float lightType = 0.0f;
        switch (light.type) {
        case LightType::Directional:
            packed.dirs[i][0] = -finiteOr(light.direction.x, 0.0f);
            packed.dirs[i][1] = -finiteOr(light.direction.y, -1.0f);
            packed.dirs[i][2] = -finiteOr(light.direction.z, 0.0f);
            break;
        case LightType::Point:
            lightType = 1.0f;
            packed.dirs[i][0] = finiteOr(light.position.x, 0.0f);
            packed.dirs[i][1] = finiteOr(light.position.y, 0.0f);
            packed.dirs[i][2] = finiteOr(light.position.z, 0.0f);
            packed.params[i][0] = std::max(finiteOr(light.range, 1.0f), 0.0001f);
            packed.params[i][1] = std::max(finiteOr(light.intensity, 0.0f), 0.0f);
            break;
        case LightType::Spot: {
            lightType = 2.0f;
            packed.dirs[i][0] = finiteOr(light.position.x, 0.0f);
            packed.dirs[i][1] = finiteOr(light.position.y, 0.0f);
            packed.dirs[i][2] = finiteOr(light.position.z, 0.0f);
            packed.params[i][0] = std::max(finiteOr(light.range, 1.0f), 0.0001f);
            packed.params[i][1] = std::max(finiteOr(light.intensity, 0.0f), 0.0f);
            const float outer = std::clamp(
                finiteOr(light.coneCosOuter, 0.0f), -1.0f, 0.9999f);
            packed.params[i][3] = outer;
            packed.params[i][2] = std::max(
                std::clamp(finiteOr(light.coneCosInner, 1.0f), -1.0f, 1.0f),
                outer + 0.0001f);
            packed.spotDirs[i][0] = finiteOr(light.spotDirection.x, 0.0f);
            packed.spotDirs[i][1] = finiteOr(light.spotDirection.y, -1.0f);
            packed.spotDirs[i][2] = finiteOr(light.spotDirection.z, 0.0f);
            break;
        }
        }
        packed.dirs[i][3] = lightType;
        packed.colors[i][0] = std::max(finiteOr(light.color.x, 0.0f), 0.0f);
        packed.colors[i][1] = std::max(finiteOr(light.color.y, 0.0f), 0.0f);
        packed.colors[i][2] = std::max(finiteOr(light.color.z, 0.0f), 0.0f);
    }

    return packed;
}

PackedShadowAtlas packShadowAtlas(const ShadowPass* shadowPass,
                                  bool receivesShadow) noexcept
{
    PackedShadowAtlas packed{};
    initializeShadowMatrices(packed);
    if (!receivesShadow || shadowPass == nullptr
        || !shadowPass->hasSampleableShadow()) {
        return packed;
    }

    packed.activeShadowCount = std::min(
        shadowPass->perLightShadowCount(), kMaxSceneLights);
    if (packed.activeShadowCount == 0u) {
        return packed;
    }

    const float* rects = shadowPass->shadowSampleRects();
    const float* matrices = shadowPass->atlasLightViewProjsColumnMajor();
    const float* biases = shadowPass->atlasShadowBiases();
    std::memcpy(packed.rects, rects, sizeof(packed.rects));
    std::memcpy(packed.lightViewProjs, matrices, sizeof(packed.lightViewProjs));
    for (uint32_t i = 0; i < kMaxSceneLights; ++i) {
        packed.biases[i][0] = biases[i];
    }
    return packed;
}

bool uploadSceneLighting(ayt::shader::ShaderResource& shader,
                         const PackedSceneLighting& packed)
{
    const bool dirs = uploadVec4Array(
        shader, "dirs", &packed.dirs[0][0], sizeof(packed.dirs));
    const bool colors = uploadVec4Array(
        shader, "colors", &packed.colors[0][0], sizeof(packed.colors));
    const bool params = uploadVec4Array(
        shader, "params", &packed.params[0][0], sizeof(packed.params));
    const bool spots = uploadVec4Array(
        shader, "spotDir", &packed.spotDirs[0][0], sizeof(packed.spotDirs));

    const ayt::shader::BindingId countBinding =
        shader.getUniformBinding("activeLightCount");
    if (countBinding != ayt::shader::InvalidBinding) {
        const float count[4] = {
            static_cast<float>(packed.activeLightCount), 0.0f, 0.0f, 0.0f
        };
        shader.setUniform(countBinding, count, sizeof(count));
    }
    return dirs && colors && params && spots;
}

bool uploadShadowAtlas(ayt::shader::ShaderResource& shader,
                       const PackedShadowAtlas& packed)
{
    const bool rects = uploadVec4Array(
        shader, "shadowAtlasRects", &packed.rects[0][0], sizeof(packed.rects));
    const bool biases = uploadVec4Array(
        shader, "shadowBiases", &packed.biases[0][0], sizeof(packed.biases));
    const bool matrices = uploadVec4Array(
        shader, "lightViewProjs", &packed.lightViewProjs[0][0],
        sizeof(packed.lightViewProjs));

    const ayt::shader::BindingId countBinding =
        shader.getUniformBinding("perLightShadowCount");
    if (countBinding != ayt::shader::InvalidBinding) {
        const float count[4] = {
            static_cast<float>(packed.activeShadowCount), 0.0f, 0.0f, 0.0f
        };
        shader.setUniform(countBinding, count, sizeof(count));
    }
    return rects && biases && matrices;
}

} // namespace ayt::render::detail
