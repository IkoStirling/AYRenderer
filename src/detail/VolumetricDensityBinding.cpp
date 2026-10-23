#include "detail/VolumetricDensityBinding.h"
#include "detail/PassExecContext.h"
namespace ayt::render::detail {
bool bindVolumetricDensity(PassExecContext& ctx,shader::ShaderResource& program,bgfx::TextureHandle fallback){
 const auto& binding=ctx.frame.densityField;auto texture=fallback;float dims[4]={1,1,1,0};
 if(binding.profile){const auto found=ctx.textures.find(binding.texture.id);if(found==ctx.textures.end()||!BGFXAdapter::isValid(found->second.handle))return false;texture=found->second.handle;dims[0]=binding.profile->width();dims[1]=binding.profile->height();dims[2]=binding.profile->depth();dims[3]=1;}
 const auto uniform=program.getUniformBinding("fogDensityGrid"),sampler=program.getTextureBinding("fogDensityAtlas");
 if(uniform==shader::InvalidBinding||sampler==shader::InvalidBinding||!BGFXAdapter::isValid(texture))return false;
 program.setUniform(uniform,dims,16);program.setTexture(program.getTextureStage(sampler),sampler,toShaderTexture(texture));return true;
}
}
