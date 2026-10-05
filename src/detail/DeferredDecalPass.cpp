#include "detail/DeferredDecalPass.h"
#include "AYRenderer/DeferredDecalShaderSources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/BgfxMatrix.h"
#include "detail/FgResource.h"
namespace ayt::render::detail {
bool DeferredDecalPass::prepare(BGFXAdapter& adapter,uint16_t width,uint16_t height,bgfx::TextureHandle depth,uint32_t generation) {
    if(!adapter.isInitialized()||adapter.isNoopBackend()||!width||!height||!BGFXAdapter::isValid(depth))return false;
    if(width==_width&&height==_height&&depth.idx==_depth.idx&&generation==_generation&&BGFXAdapter::isValid(_targets[0])&&BGFXAdapter::isValid(_targets[1]))return true;
    destroyResources(adapter);
    for(size_t t=0;t<2;++t) {
        for(size_t i=0;i<4;++i) {
            auto format=i==2?bgfx::TextureFormat::RGBA16F:bgfx::TextureFormat::RGBA8;
            _colors[t][i]=bgfx::createTexture2D(width,height,false,1,format,BGFX_TEXTURE_RT|BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP);
            if(!BGFXAdapter::isValid(_colors[t][i])){destroyResources(adapter);return false;}
        }
        bgfx::TextureHandle attachments[5]={_colors[t][0],_colors[t][1],_colors[t][2],_colors[t][3],depth};
        _targets[t]=bgfx::createFrameBuffer(5,attachments,false); // depth is borrowed, never owned
        if(!BGFXAdapter::isValid(_targets[t])){destroyResources(adapter);return false;}
    }
    _width=width;_height=height;_depth=depth;_generation=generation;return true;
}
uint32_t DeferredDecalPass::execute(PassExecContext& ctx) {
    if(!ctx.frame.decalsEnabled||!ctx.decals||ctx.decals->empty()||ctx.decals->size()>kMaxDeferredDecals||!ctx.resourceBlackboard||!ctx.frameGraph||!ctx.adapter.isInitialized()||ctx.adapter.isNoopBackend()||!ctx.viewportWidth||!ctx.viewportHeight)return 0;
    BlackboardGBufferView gb;
    if(!ctx.resourceBlackboard->resolveGBuffer(gb)||!BGFXAdapter::isValid(_targets[0])||!BGFXAdapter::isValid(_targets[1])||gb.depth.idx!=_depth.idx||gb.generation!=_generation||gb.width!=_width||gb.height!=_height)return 0;
    if(!_geometry.ensure(ctx.adapter))return 0;
    if(!_program.isValid())_program=ctx.pool.acquire(kDeferredDecalPhoskiaSource,kDeferredDecalCacheKey);
    if(!_program.isValid())return 0;
    // Validate the complete transaction before the first draw. Missing resources
    // must not publish a partial/parity-mismatched GBuffer.
    for(const auto& d:*ctx.decals) {
        if(d.opacity<=0||!validDeferredDecalTransform(d.localToWorld))return 0;
        for(auto h:{d.albedo,d.normal})if(h.isValid()) {
            auto it=ctx.textures.find(h.id);if(it==ctx.textures.end()||!BGFXAdapter::isValid(it->second.handle)||it->second.cube)return 0;
        }
    }
    std::array<bgfx::TextureHandle,4> source={gb.albedo,gb.normal,gb.worldPosition,gb.material};
    for(size_t i=0;i<ctx.decals->size();++i) {
        const auto& d=(*ctx.decals)[i];bool bound=true;
        auto tex=[&](const char* n,bgfx::TextureHandle h){auto b=_program.getTextureBinding(n);bound&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(h));};
        auto u=[&](const char* n,const void* p,uint32_t bytes){auto b=_program.getUniformBinding(n);bound&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setUniform(b,p,bytes);};
        const char* names[4]={"sourceAlbedo","sourceNormal","sourceWorld","sourceSurface"};for(int c=0;c<4;++c)tex(names[c],source[c]);
        tex("decalAlbedo",d.albedo.isValid()?ctx.textures.at(d.albedo.id).handle:source[0]);
        tex("decalNormal",d.normal.isValid()?ctx.textures.at(d.normal.id).handle:source[1]);
        float inverse[16];toBgfxColumnMajor(d.localToWorld.inverse(),inverse);u("decalWorldToLocal",inverse,64);
        auto axis=[&](int c){math::FVector3 a{d.localToWorld.row[0][c],d.localToWorld.row[1][c],d.localToWorld.row[2][c]};return a/a.length();};
        auto T=axis(0),B=axis(1),N=axis(2);
        const float tangent[4]={T.x,T.y,T.z,0},bitangent[4]={B.x,B.y,B.z,0},facing[4]={N.x,N.y,N.z,d.minFacing};
        const float tint[4]={d.color.x,d.color.y,d.color.z,d.opacity},material[4]={d.roughness,d.metallic,d.materialStrength,d.normalStrength};
        const float options[4]={d.albedo.isValid()?1.f:0.f,d.normal.isValid()?1.f:0.f,0,0};
        u("decalTangent",tangent,16);u("decalBitangent",bitangent,16);u("decalFacing",facing,16);u("decalTint",tint,16);u("decalMaterial",material,16);u("decalOptions",options,16);
        if(!bound)return 0;
        const uint8_t view=kFirstView+static_cast<uint8_t>(i);auto target=_targets[i%2];
        configureFullscreenPassView(ctx.adapter,view,target,0,0,_width,_height);
        _geometry.bind(ctx.adapter);ctx.adapter.setStateDepthTestAlways();shader::DrawCallContext draw;draw.viewId=view;draw.state=0;_program.submit(draw);
        source=_colors[i%2];
    }
    auto target=output(ctx.decals->size());
    ctx.resourceBlackboard->publishGBuffer(target,source[0],source[1],source[2],source[3],gb.depth,_width,_height,_generation,true);
    for(auto id:{FgResourceId::GBufferAlbedo,FgResourceId::GBufferNormal,FgResourceId::GBufferWorldPosition,FgResourceId::GBufferSurface})ctx.frameGraph->markProduced(id);
    return static_cast<uint32_t>(ctx.decals->size());
}
void DeferredDecalPass::destroyResources(BGFXAdapter& adapter) {
    _program.reset();_geometry.destroy(adapter);
    for(auto& f:_targets){if(BGFXAdapter::isValid(f))bgfx::destroy(f);f=BGFX_INVALID_HANDLE;}
    for(auto& group:_colors)for(auto& h:group){if(BGFXAdapter::isValid(h))bgfx::destroy(h);h=BGFX_INVALID_HANDLE;}
    _width=_height=0;_generation=0;_depth=BGFX_INVALID_HANDLE;
}
}
