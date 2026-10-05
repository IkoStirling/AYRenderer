#pragma once
#include "detail/RenderPass.h"
#include "detail/FullscreenPassGeometry.h"
#include "AYShader/ShaderResource.h"
#include <array>
namespace ayt::render::detail {
class DeferredDecalPass final:public RenderPass {
public:
    DeferredDecalPass(){for(auto& group:_colors)for(auto& h:group)h=BGFX_INVALID_HANDLE;}
    static constexpr uint8_t kFirstView=34;
    std::string_view name() const override{return "DeferredDecal";}
    bool prepare(BGFXAdapter& adapter,uint16_t width,uint16_t height,bgfx::TextureHandle depth,uint32_t generation);
    bgfx::FrameBufferHandle output(size_t count) const noexcept{return count?_targets[(count-1)%2]:bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};}
    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter);
private:
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    std::array<bgfx::FrameBufferHandle,2> _targets{{BGFX_INVALID_HANDLE,BGFX_INVALID_HANDLE}};
    std::array<std::array<bgfx::TextureHandle,4>,2> _colors{};
    bgfx::TextureHandle _depth=BGFX_INVALID_HANDLE;
    uint16_t _width=0,_height=0;
    uint32_t _generation=0;
};
}
