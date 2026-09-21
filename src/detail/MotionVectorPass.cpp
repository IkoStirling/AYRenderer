#include "detail/MotionVectorPass.h"

#include "AYRenderer/RenderTypes.h"
#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/FrameDrawLists.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/RasterConvention.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ayt::render::detail
{

namespace
{

// Motion-vector shaders are authored in Phoskia like the rest of the
// production render pipeline. `color` and `tangent` provide two vec4
// interpolators for the current and previous clip positions; their names are
// engine semantics here, not material color/tangent data.
constexpr const char* kMotionVectorPhoskiaSource = R"(
material MotionVector {
    // Keep the palettes as reflected arrays. A mixed cbuffer containing two
    // mat4 arrays has no field-level bgfx bindings on D3D.
    uniform mat4 bones[128]
    uniform mat4 previousBones[128]
    uniform mat4 previousWorld
    uniform mat4 previousViewProjection
    // x = skinned draw, y = previous object/frame history is valid.
    uniform vec4 motionParams
    uniform vec4 motionJitter
    vertex {
        in pos : position
        in boneId : boneindices
        in boneWt : boneweights
        out currentClip : color = modelViewProjection * mix(
            vec4(pos, 1.0),
            skinningMatrix(boneId, boneWt, bones,
                           vec4(pos, 1.0)),
            motionParams.x)
        out previousClip : tangent = previousViewProjection *
            (previousWorld * mix(
                vec4(pos, 1.0),
                skinningMatrix(boneId, boneWt, previousBones,
                               vec4(pos, 1.0)),
                motionParams.x))
        return currentClip
    }
    fragment {
        in currentClip : color
        in previousClip : tangent
        // Derive on the rasterized primitive, before divergent control flow.
        // Unlike depth-texture derivatives, helper lanes do not mix another
        // object's depth into the surface's pixel footprint at silhouettes.
        let previousDepth = previousClip.z / max(previousClip.w, 0.00001)
        let previousDepthFootprint = min(fwidth(previousDepth), 1.0)
        let currentNdc = currentClip.xy / max(abs(currentClip.w), 0.00001)
        let previousNdc = previousClip.xy / max(abs(previousClip.w), 0.00001)
        let currentUv = vec2(currentNdc.x * 0.5 + 0.5,
                             1.0 - (currentNdc.y * 0.5 + 0.5))
        let previousUv = vec2(previousNdc.x * 0.5 + 0.5,
                              1.0 - (previousNdc.y * 0.5 + 0.5))
        // xy excludes jitter; z tracks the previous *deformed* surface.
        let velocity = vec2(2.0, 2.0)
        let valid = 0.0
        if (motionParams.y > 0.5 && previousClip.w > 0.00001) {
            // Keep each subtraction explicit: Phoskia currently associates
            // an unparenthesized a-b-c+d chain from the right. That leaves
            // twice the current jitter in a stationary surface's velocity.
            let unjitteredCurrentUv = currentUv - motionJitter.xy
            let unjitteredPreviousUv = previousUv - motionJitter.zw
            velocity = unjitteredCurrentUv - unjitteredPreviousUv
            valid = 1.0 + previousDepthFootprint
        }
        return vec4(velocity, previousDepth, valid)
    }
}
)";

constexpr const char* kMotionVectorCutoutPhoskiaSource = R"(
material MotionVectorCutout {
    // See the opaque variant: each palette needs its own bgfx binding.
    uniform mat4 bones[128]
    uniform mat4 previousBones[128]
    texture2d albedoMap
    texture2d opacityMap
    uniform vec4 baseColor
    uniform vec4 alphaCutoff
    uniform vec4 opacity
    uniform vec4 opacitySource
    uniform mat4 previousWorld
    uniform mat4 previousViewProjection
    uniform vec4 motionParams
    uniform vec4 motionJitter
    vertex {
        in pos : position
        in uv : texcoord
        in boneId : boneindices
        in boneWt : boneweights
        out currentClip : color = modelViewProjection * mix(
            vec4(pos, 1.0),
            skinningMatrix(boneId, boneWt, bones,
                           vec4(pos, 1.0)),
            motionParams.x)
        out previousClip : tangent = previousViewProjection *
            (previousWorld * mix(
                vec4(pos, 1.0),
                skinningMatrix(boneId, boneWt, previousBones,
                               vec4(pos, 1.0)),
                motionParams.x))
        out vUv : texcoord = uv
        return currentClip
    }
    fragment {
        in currentClip : color
        in previousClip : tangent
        in vUv : texcoord
        // Must precede alpha discard so derivatives remain well-defined.
        let previousDepth = previousClip.z / max(previousClip.w, 0.00001)
        let previousDepthFootprint = min(fwidth(previousDepth), 1.0)
        let albedo = sample(albedoMap, vUv) * baseColor
        let opacitySample = sample(opacityMap, vUv)
        let dedicatedOpacity = mix(opacitySample.x, opacitySample.w,
                                   step(1.5, opacitySource.x))
        let sampledOpacity = mix(1.0, dedicatedOpacity,
                                 step(0.5, opacitySource.x))
        let surfaceAlpha = albedo.w * sampledOpacity
                         * clamp(opacity.x, 0.0, 1.0)
        if (surfaceAlpha < alphaCutoff.x) {
            discard
        }
        let currentNdc = currentClip.xy / max(abs(currentClip.w), 0.00001)
        let previousNdc = previousClip.xy / max(abs(previousClip.w), 0.00001)
        let currentUv = vec2(currentNdc.x * 0.5 + 0.5,
                             1.0 - (currentNdc.y * 0.5 + 0.5))
        let previousUv = vec2(previousNdc.x * 0.5 + 0.5,
                              1.0 - (previousNdc.y * 0.5 + 0.5))
        let velocity = vec2(2.0, 2.0)
        let valid = 0.0
        if (motionParams.y > 0.5 && previousClip.w > 0.00001) {
            let unjitteredCurrentUv = currentUv - motionJitter.xy
            let unjitteredPreviousUv = previousUv - motionJitter.zw
            velocity = unjitteredCurrentUv - unjitteredPreviousUv
            valid = 1.0 + previousDepthFootprint
        }
        return vec4(velocity, previousDepth, valid)
    }
}
)";

constexpr const char* kMotionVectorCacheKey =
    "motion_vector_phoskia_rgba16f_depth_footprint_v6";
constexpr const char* kMotionVectorCutoutCacheKey =
    "motion_vector_phoskia_rgba16f_depth_footprint_cutout_v6";

struct CommitKey final {
    uint64_t objectId = 0;
    uint64_t meshId = 0;
    bool operator==(const CommitKey& rhs) const noexcept {
        return objectId == rhs.objectId && meshId == rhs.meshId;
    }
};

struct CommitKeyHash final {
    std::size_t operator()(const CommitKey& key) const noexcept
    {
        const std::size_t a = std::hash<uint64_t>{}(key.objectId);
        const std::size_t b = std::hash<uint64_t>{}(key.meshId);
        return a ^ (b + static_cast<std::size_t>(0x9e3779b9u)
                    + (a << 6u) + (a >> 2u));
    }
};

uint32_t completeSkeletonCount(const DrawItem& item) noexcept
{
    if (item.boneMatrices == nullptr || item.jointCount == 0u) {
        return 0u;
    }
    return item.skeletonJointCount != 0u
        ? item.skeletonJointCount
        : item.jointCount;
}

bool bonePaletteIsValid(const DrawItem& item,
                        const ayt::math::Float4x4* sourceBones,
                        uint32_t sourceBoneCount)
{
    if (sourceBones == nullptr || item.jointCount == 0u
        || item.jointCount > ayt::render::kUniformSkinPaletteCapacity) {
        return false;
    }
    if (item.boneRemap != nullptr) {
        if (sourceBoneCount == 0u) {
            return false;
        }
        for (uint32_t slot = 0; slot < item.jointCount; ++slot) {
            if (item.boneRemap[slot] >= sourceBoneCount) {
                return false;
            }
        }
    } else if (sourceBoneCount < item.jointCount) {
        return false;
    }
    return true;
}

bool uploadBonePalette(
    ayt::shader::ShaderResource& program,
    const char* uniformName,
    const DrawItem& item,
    const ayt::math::Float4x4* sourceBones,
    uint32_t sourceBoneCount)
{
    if (uniformName == nullptr
        || !bonePaletteIsValid(item, sourceBones, sourceBoneCount)) {
        return false;
    }
    const ayt::shader::BindingId binding =
        program.getUniformBinding(uniformName);
    if (binding == ayt::shader::InvalidBinding) {
        return false;
    }
    std::vector<float> matrices(
        static_cast<std::size_t>(item.jointCount) * 16u);
    for (uint32_t slot = 0; slot < item.jointCount; ++slot) {
        const uint32_t joint = item.boneRemap != nullptr
            ? item.boneRemap[slot]
            : slot;
        toBgfxColumnMajor(sourceBones[joint], &matrices[slot * 16u]);
    }
    program.setUniform(binding, matrices.data(),
                       matrices.size() * sizeof(float));
    return true;
}

bool bindCutoutTextures(ayt::shader::ShaderResource& program,
                        BGFXAdapter& adapter,
                        const GpuMaterial& material,
                        const std::unordered_map<uint64_t, GpuTexture>& textures)
{
    const ayt::shader::BindingId albedoBinding =
        program.getTextureBinding("albedoMap");
    const ayt::shader::BindingId opacityBinding =
        program.getTextureBinding("opacityMap");
    if (albedoBinding == ayt::shader::InvalidBinding
        || opacityBinding == ayt::shader::InvalidBinding) {
        return false;
    }

    bool albedoBound = false;
    bool opacityBound = false;
    for (const GpuMaterial::TextureSlot& slot : material.textures) {
        if (!slot.texture.isValid()) {
            continue;
        }
        ayt::shader::BindingId target = ayt::shader::InvalidBinding;
        if (slot.name == "albedoMap" || slot.name == "baseColorTexture"
            || slot.name == "diffuse" || slot.name == "mainTexture"
            || slot.name == "albedo") {
            target = albedoBinding;
        } else if (slot.name == "opacityMap"
                   || slot.name == "opacityTexture") {
            target = opacityBinding;
        }
        if (target == ayt::shader::InvalidBinding) {
            continue;
        }
        const auto textureIt = textures.find(slot.texture.id);
        if (textureIt == textures.end()
            || !BGFXAdapter::isValid(textureIt->second.handle)) {
            continue;
        }
        program.setTexture(program.getTextureStage(target), target,
                           toShaderTexture(textureIt->second.handle));
        albedoBound |= target == albedoBinding;
        opacityBound |= target == opacityBinding;
    }
    tryBindWhiteTexture(program, adapter, "albedoMap", albedoBound);
    tryBindWhiteTexture(program, adapter, "opacityMap", opacityBound);
    return true;
}

bool hasRequiredBindings(const ayt::shader::ShaderResource& program,
                         bool cutout) noexcept
{
    const bool common = program.isValid()
        && program.getUniformBinding("bones") != ayt::shader::InvalidBinding
        && program.getUniformBinding("previousBones")
               != ayt::shader::InvalidBinding
        && program.getUniformBinding("previousWorld") != ayt::shader::InvalidBinding
        && program.getUniformBinding("previousViewProjection") != ayt::shader::InvalidBinding
        && program.getUniformBinding("motionParams") != ayt::shader::InvalidBinding
        && program.getUniformBinding("motionJitter") != ayt::shader::InvalidBinding;
    if (!common || !cutout) {
        return common;
    }
    return program.getTextureBinding("albedoMap") != ayt::shader::InvalidBinding
        && program.getTextureBinding("opacityMap") != ayt::shader::InvalidBinding
        && program.getUniformBinding("baseColor") != ayt::shader::InvalidBinding
        && program.getUniformBinding("alphaCutoff") != ayt::shader::InvalidBinding
        && program.getUniformBinding("opacity") != ayt::shader::InvalidBinding
        && program.getUniformBinding("opacitySource") != ayt::shader::InvalidBinding;
}

} // namespace

const char* const kMotionVectorCacheKeyCStr = kMotionVectorCacheKey;
const char* const kMotionVectorCutoutCacheKeyCStr =
    kMotionVectorCutoutCacheKey;
const char* motionVectorPhoskiaSourceForTests() noexcept {
    return kMotionVectorPhoskiaSource;
}
const char* motionVectorCutoutPhoskiaSourceForTests() noexcept {
    return kMotionVectorCutoutPhoskiaSource;
}

std::size_t MotionHistoryCache::KeyHash::operator()(const Key& key) const noexcept
{
    const std::size_t a = std::hash<uint64_t>{}(key.objectId);
    const std::size_t b = std::hash<uint64_t>{}(key.meshId);
    return a ^ (b + static_cast<std::size_t>(0x9e3779b9u)
                + (a << 6u) + (a >> 2u));
}

const MotionHistorySnapshot* MotionHistoryCache::find(
    uint64_t objectId,
    uint64_t meshId,
    uint32_t skeletonJointCount,
    uint64_t currentFrame) const noexcept
{
    if (objectId == 0u || meshId == 0u || currentFrame == 0u) {
        return nullptr;
    }
    const auto it = _entries.find(Key{objectId, meshId});
    if (it == _entries.end()
        || it->second.lastSeenFrame != currentFrame - 1u
        || it->second.bones.size() != skeletonJointCount) {
        return nullptr;
    }
    return &it->second;
}

void MotionHistoryCache::commit(
    uint64_t objectId,
    uint64_t meshId,
    const ayt::math::Float4x4& world,
    const ayt::math::Float4x4* bones,
    uint32_t skeletonJointCount,
    uint64_t currentFrame)
{
    if (objectId == 0u || meshId == 0u || currentFrame == 0u) {
        return;
    }
    MotionHistorySnapshot& snapshot = _entries[Key{objectId, meshId}];
    snapshot.world = world;
    snapshot.lastSeenFrame = currentFrame;
    if (bones != nullptr && skeletonJointCount != 0u) {
        snapshot.bones.assign(bones, bones + skeletonJointCount);
    } else {
        snapshot.bones.clear();
    }
}

void MotionHistoryCache::pruneBefore(uint64_t oldestFrame)
{
    for (auto it = _entries.begin(); it != _entries.end();) {
        if (it->second.lastSeenFrame < oldestFrame) {
            it = _entries.erase(it);
        } else {
            ++it;
        }
    }
}

void MotionVectorPass::setRequestedThisFrame(bool requested) noexcept
{
    if (requested != _wasRequestedLastFrame) {
        invalidateHistory();
    }
    _requestedThisFrame = requested;
    _wasRequestedLastFrame = requested;
}

void MotionVectorPass::invalidateHistory() noexcept
{
    _history.clear();
    _previousViewProjection = ayt::math::Float4x4::identity();
    _frameSerial = 0;
    _previousJitterX = 0.0f;
    _previousJitterY = 0.0f;
    _hasPreviousFrame = false;
    _producedThisFrame = false;
}

bool MotionVectorPass::programsReady() const noexcept
{
    return hasRequiredBindings(_opaqueProgram, /*cutout=*/false)
        && hasRequiredBindings(_cutoutProgram, /*cutout=*/true);
}

void MotionVectorPass::ensurePrograms(ayt::shader::ShaderResourcePool& pool)
{
    if (!_opaqueProgram.isValid() && !_opaqueAcquireFailed) {
        _opaqueProgram = pool.acquire(
            kMotionVectorPhoskiaSource, kMotionVectorCacheKey);
        _opaqueAcquireFailed = !_opaqueProgram.isValid();
        if (_opaqueAcquireFailed) {
            std::fprintf(stderr, "[MotionVectorPass] opaque shader acquire failed\n");
            for (const std::string& error : pool.lastCompileErrors()) {
                std::fprintf(stderr, "[MotionVectorPass]   %s\n", error.c_str());
            }
        }
    }
    if (!_cutoutProgram.isValid() && !_cutoutAcquireFailed) {
        _cutoutProgram = pool.acquire(
            kMotionVectorCutoutPhoskiaSource,
            kMotionVectorCutoutCacheKey);
        _cutoutAcquireFailed = !_cutoutProgram.isValid();
        if (_cutoutAcquireFailed) {
            std::fprintf(stderr, "[MotionVectorPass] cutout shader acquire failed\n");
            for (const std::string& error : pool.lastCompileErrors()) {
                std::fprintf(stderr, "[MotionVectorPass]   %s\n", error.c_str());
            }
        }
    }
}

void MotionVectorPass::ensureResources(BGFXAdapter& adapter,
                                       bgfx::TextureHandle gbufferDepth)
{
    if (_allocatedWidth == _requestedWidth
        && _allocatedHeight == _requestedHeight
        && BGFXAdapter::isValid(_velocityTexture)
        && BGFXAdapter::isValid(_velocityFbo)
        && BGFXAdapter::isValid(_borrowedDepth)
        && _borrowedDepth.idx == gbufferDepth.idx) {
        return;
    }
    destroyTarget(adapter);
    if (_requestedWidth == 0u || _requestedHeight == 0u
        || !BGFXAdapter::isValid(gbufferDepth)) {
        return;
    }
    const uint64_t samplerFlags = BGFX_SAMPLER_U_CLAMP
        | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT
        | BGFX_SAMPLER_MAG_POINT;
    _velocityTexture = adapter.createRenderTargetTexture2D(
        _requestedWidth, _requestedHeight, kVelocityFormat, samplerFlags);
    if (BGFXAdapter::isValid(_velocityTexture)) {
        _velocityFbo = adapter.createBorrowedColorDepthFrameBuffer(
            _velocityTexture, gbufferDepth);
    }
    if (!BGFXAdapter::isValid(_velocityTexture)
        || !BGFXAdapter::isValid(_velocityFbo)) {
        destroyTarget(adapter);
        return;
    }
    _borrowedDepth = gbufferDepth;
    _allocatedWidth = _requestedWidth;
    _allocatedHeight = _requestedHeight;
    ++_targetGeneration;
    if (_targetGeneration == 0) {
        ++_targetGeneration;
    }
    invalidateHistory();
}

uint32_t MotionVectorPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;
    if (!_requestedThisFrame) {
        return 0u;
    }
    if (!isEnabled()) {
        rateLimitedEarlyReturn("MotionVectorPass", "requested pass disabled");
        return 0u;
    }
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        rateLimitedEarlyReturn("MotionVectorPass", "backend unavailable");
        return 0u;
    }
    if (ctx.gbufferPass == nullptr
        || !ctx.gbufferPass->producedThisFrame()
        || !ctx.gbufferPass->hasValidAttachments()) {
        rateLimitedEarlyReturn("MotionVectorPass", "current GBuffer unavailable");
        return 0u;
    }

    const bgfx::TextureHandle gbufferDepth = ctx.gbufferPass->gbufferDepthRt();
    ensureResources(ctx.adapter, gbufferDepth);
    ensurePrograms(ctx.pool);
    if (!BGFXAdapter::isValid(_velocityTexture)
        || !BGFXAdapter::isValid(_velocityFbo)) {
        rateLimitedEarlyReturn(
            "MotionVectorPass", "RGBA16F target or borrowed-depth FBO unavailable");
        return 0u;
    }
    if (!programsReady()) {
        rateLimitedEarlyReturn("MotionVectorPass", "shader bindings unavailable");
        return 0u;
    }

    ++_frameSerial;
    const ayt::math::Float4x4 currentViewProjection =
        ctx.frame.projection * ctx.frame.view;
    const ayt::math::Float4x4 previousViewProjection = _hasPreviousFrame
        ? _previousViewProjection
        : currentViewProjection;
    float previousVp[16];
    toBgfxColumnMajor(previousViewProjection, previousVp);

    constexpr uint8_t viewId = kMotionVectorViewId;
    ctx.adapter.setViewMode(viewId, bgfx::ViewMode::Sequential);
    ctx.adapter.setViewTransform(viewId, ctx.frame.view, ctx.frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _velocityFbo);
    ctx.adapter.setViewRect(viewId, 0, 0,
                            _allocatedWidth, _allocatedHeight);
    // Borrowed GBuffer depth must survive. Alpha zero marks every skipped
    // draw (including WorldLit2D) invalid instead of publishing zero motion.
    ctx.adapter.setViewClearRaw(viewId, BGFX_CLEAR_COLOR, 0x00000000u);
    ctx.adapter.touch(viewId);

    std::unordered_map<CommitKey, const DrawItem*, CommitKeyHash> commits;
    uint32_t drawCount = 0u;
    FrameDrawLists fallbackDrawLists;
    const FrameDrawLists& drawLists =
        resolveFrameDrawLists(ctx, fallbackDrawLists);
    for (const DrawItem* itemPtr : drawLists.opaque3D) {
        const DrawItem& item = *itemPtr;
        const auto meshIt = ctx.meshes.find(item.mesh.id);
        const auto materialIt = ctx.materials.find(item.material.id);
        if (meshIt == ctx.meshes.end() || materialIt == ctx.materials.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second;
        const GpuMaterial& material = materialIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)
            || !material.shader.isValid()) {
            continue;
        }
        const DrawIndexRange range = resolveDrawIndexRange(item, mesh);
        if (range.indexCount == 0u) {
            continue;
        }

        ayt::shader::ShaderResource& program = material.alphaCutout
            ? _cutoutProgram
            : _opaqueProgram;
        const uint32_t currentBoneCount = completeSkeletonCount(item);
        const bool currentSkinned = bonePaletteIsValid(
            item, item.boneMatrices, currentBoneCount);

        const MotionHistorySnapshot* previous = nullptr;
        bool historyValid = _hasPreviousFrame && item.motionObjectId != 0u;
        if (item.motionObjectId != 0u) {
            previous = _history.find(item.motionObjectId, item.mesh.id,
                                     currentBoneCount, _frameSerial);
            historyValid = historyValid && previous != nullptr;
        }
        const ayt::math::Float4x4& previousWorld = previous != nullptr
            ? previous->world
            : item.world;
        const ayt::math::Float4x4* previousBones = previous != nullptr
            && !previous->bones.empty()
            ? previous->bones.data()
            : item.boneMatrices;
        const uint32_t previousBoneCount = previous != nullptr
            ? static_cast<uint32_t>(previous->bones.size())
            : currentBoneCount;
        if (currentSkinned
            && (!uploadBonePalette(
                    program, "bones", item,
                    item.boneMatrices, currentBoneCount)
                || !uploadBonePalette(
                    program, "previousBones", item,
                    previousBones, previousBoneCount))) {
            // Do not publish rigid velocity for a skinned surface when its
            // complete current/previous palette could not be uploaded.
            continue;
        }

        trySetUniformMat4(program, "previousWorld", nullptr, previousWorld);
        program.setUniform(
            program.getUniformBinding("previousViewProjection"),
            previousVp, sizeof(previousVp));
        const float motionParams[4] = {
            currentSkinned ? 1.0f : 0.0f,
            historyValid ? 1.0f : 0.0f,
            0.0f, 0.0f,
        };
        program.setUniform(program.getUniformBinding("motionParams"),
                           motionParams, sizeof(motionParams));
        const float motionJitter[4] = {
            _currentJitterX, _currentJitterY,
            _previousJitterX, _previousJitterY,
        };
        program.setUniform(program.getUniformBinding("motionJitter"),
                           motionJitter, sizeof(motionJitter));

        if (material.alphaCutout) {
            const float baseColor[4] = {
                material.hasColorOverride ? material.colorOverride.x : 1.0f,
                material.hasColorOverride ? material.colorOverride.y : 1.0f,
                material.hasColorOverride ? material.colorOverride.z : 1.0f,
                material.hasColorOverride ? material.colorOverride.w : 1.0f,
            };
            program.setUniform(program.getUniformBinding("baseColor"),
                               baseColor, sizeof(baseColor));
            const float alphaCutoff[4] = {
                material.alphaCutoff, 0.0f, 0.0f, 0.0f
            };
            program.setUniform(program.getUniformBinding("alphaCutoff"),
                               alphaCutoff, sizeof(alphaCutoff));
            const float opacity[4] = {
                materialUniformScalar(material, "opacity", 1.0f),
                0.0f, 0.0f, 0.0f
            };
            const float opacitySource[4] = {
                materialUniformScalar(material, "opacitySource", 0.0f),
                0.0f, 0.0f, 0.0f
            };
            program.setUniform(program.getUniformBinding("opacity"),
                               opacity, sizeof(opacity));
            program.setUniform(program.getUniformBinding("opacitySource"),
                               opacitySource, sizeof(opacitySource));
            if (!bindCutoutTextures(program, ctx.adapter,
                                    material, ctx.textures)) {
                continue;
            }
        }

        ctx.adapter.setStateColorLEQUAL(
            material.doubleSided, reversesWinding(item.world));
        ctx.adapter.setTransform(item.world);
        ctx.adapter.setVertexBuffer(mesh.vertexBuffer);
        ctx.adapter.setIndexBuffer(mesh.indexBuffer,
                                   range.firstIndex, range.indexCount);
        ayt::shader::DrawCallContext draw;
        draw.viewId = viewId;
        draw.state = 0;
        program.submit(draw);
        ++drawCount;

        if (item.motionObjectId != 0u) {
            commits[CommitKey{item.motionObjectId, item.mesh.id}] = &item;
        }
    }

    // Publish snapshots only after every submesh has read the old frame.
    for (const auto& pair : commits) {
        const DrawItem& item = *pair.second;
        const uint32_t boneCount = completeSkeletonCount(item);
        _history.commit(item.motionObjectId, item.mesh.id, item.world,
                        item.boneMatrices, boneCount, _frameSerial);
    }
    const uint64_t oldestFrame = _frameSerial > kHistoryRetentionFrames
        ? _frameSerial - kHistoryRetentionFrames
        : 0u;
    _history.pruneBefore(oldestFrame);
    _previousViewProjection = currentViewProjection;
    _previousJitterX = _currentJitterX;
    _previousJitterY = _currentJitterY;
    _hasPreviousFrame = true;
    _producedThisFrame = true;
    if (ctx.resourceBlackboard != nullptr) {
        ctx.resourceBlackboard->publish(
            BlackboardResourceId::MotionVectors,
            BlackboardResourceLifetime::External,
            _velocityFbo, _velocityTexture,
            _allocatedWidth, _allocatedHeight,
            _targetGeneration, false);
        ctx.resourceBlackboard->markProduced(
            BlackboardResourceId::MotionVectors);
    }

    if (!_firstDispatchLogged) {
        std::fprintf(stderr,
                     "[MotionVectorPass] first dispatch view=%u size=%ux%u "
                     "format=RGBA16F draws=%u historyKeys=%zu\n",
                     static_cast<unsigned>(viewId),
                     static_cast<unsigned>(_allocatedWidth),
                     static_cast<unsigned>(_allocatedHeight),
                     static_cast<unsigned>(drawCount), _history.size());
        _firstDispatchLogged = true;
    }
    return drawCount;
}

void MotionVectorPass::destroyTarget(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_velocityFbo)) {
        adapter.destroy(_velocityFbo);
    }
    _velocityFbo = BGFX_INVALID_HANDLE;
    if (BGFXAdapter::isValid(_velocityTexture)) {
        adapter.destroy(_velocityTexture);
    }
    _velocityTexture = BGFX_INVALID_HANDLE;
    _borrowedDepth = BGFX_INVALID_HANDLE;
    _allocatedWidth = 0;
    _allocatedHeight = 0;
}

void MotionVectorPass::destroyResources(BGFXAdapter& adapter)
{
    destroyTarget(adapter);
    _opaqueProgram.reset();
    _cutoutProgram.reset();
    _opaqueAcquireFailed = false;
    _cutoutAcquireFailed = false;
    _firstDispatchLogged = false;
    invalidateHistory();
}

} // namespace ayt::render::detail
