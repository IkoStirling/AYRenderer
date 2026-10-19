#pragma once
#include "AYRenderer/ProceduralNoiseShaderSources.h"
#include <string>
namespace ayt::render {
inline constexpr char kAnalogDisplayCacheKey[]="analog_display_physical_mask_noise_v2";
inline const char* analogDisplayPhoskiaSource() {
static const std::string source=std::string(kProceduralScreenNoiseFunctions)+R"PHOSKIA(
material AnalogDisplay {
    texture2d inputColor
    uniform vec4 displayViewport
    uniform vec4 displayParams
    uniform vec4 displayFinish
    vertex {
        in pos:position
        out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5)
        return vec4(pos.xy,0.0,1.0)
    }
    fragment {
        in vUv:texcoord
        let screenUv=vec2(vUv.x,1.0-vUv.y)
        let centered=screenUv*vec2(2.0)-vec2(1.0)
        let warped=centered*(vec2(1.0)+displayParams.w*vec2(centered.y*centered.y,centered.x*centered.x))
        let uv=warped*vec2(0.5)+vec2(0.5)
        let inside=step(0.0,uv.x)*step(uv.x,1.0)*step(0.0,uv.y)*step(uv.y,1.0)
        let halfTexel=vec2(0.5)/displayViewport.xy
        let sampleUv=vec2(clamp(uv.x,halfTexel.x,1.0-halfTexel.x),clamp(uv.y,halfTexel.y,1.0-halfTexel.y))
        let source=sampleLod(inputColor,sampleUv,0.0)
        let result=source.rgb
        if(displayParams.z>0.0) {
            let separation=vec2(displayParams.z/displayViewport.x,0.0)
            let red=sampleLod(inputColor,sampleUv+separation,0.0).r
            let blue=sampleLod(inputColor,sampleUv-separation,0.0).b
            result=vec3(red,source.g,blue)
        }
        let px=floor(screenUv.x*displayViewport.x)
        let py=floor(screenUv.y*displayViewport.y)
        let oddRow=py-floor(py*0.5)*2.0
        result=result*(1.0-displayParams.x*oddRow)
        let column=px-floor(px/3.0)*3.0
        let mask=vec3(1.0-displayParams.y)
        if(column<0.5) {mask=vec3(1.0,1.0-displayParams.y,1.0-displayParams.y)}
        if(column>=0.5 && column<1.5) {mask=vec3(1.0-displayParams.y,1.0,1.0-displayParams.y)}
        if(column>=1.5) {mask=vec3(1.0-displayParams.y,1.0-displayParams.y,1.0)}
        result=result*mask
        let vignette=smoothstep(0.2,1.414214,length(centered))
        result=result*(1.0-displayFinish.x*vignette)
        if(displayFinish.y>0.0) {
            let noise=ayScreenNoise(px,py,displayFinish.z)-0.5
            result=result+vec3(noise*displayFinish.y)
        }
        result=vec3(clamp(result.r,0.0,1.0),clamp(result.g,0.0,1.0),clamp(result.b,0.0,1.0))*inside
        // Warp follows sampled alpha; scanlines, mask, grain and bezel RGB do
        // not quantize or modulate alpha.
        return vec4(result,source.a)
    }
}
)PHOSKIA";
return source.c_str();
}
} // namespace ayt::render
