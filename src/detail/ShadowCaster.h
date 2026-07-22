#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"

#include "detail/BGFXAdapter.h"
#include "detail/GpuResources.h"

#include <cstdint>
#include <unordered_map>

namespace ayt::render::detail
{

// Phase 4 — shadow depth draw loop (program acquire + cast-only iteration).
class ShadowCaster {
public:
    void destroy(BGFXAdapter& adapter);
    void ensureProgram(ayt::shader::ShaderResourcePool& pool);
    bool isProgramReady() const noexcept;

    uint32_t drawCasters(BGFXAdapter& adapter,
                         uint8_t viewId,
                         uint64_t casterState,
                         const RenderScene& scene,
                         const std::unordered_map<uint64_t, GpuMesh>& meshes);

private:
    ayt::shader::ShaderResource _program;
    ayt::shader::BindingId      _skeletonBinding = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _castSkinnedBinding = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _solidBinding = ayt::shader::InvalidBinding;
    bool                        _acquireFailed = false;
};

} // namespace ayt::render::detail
