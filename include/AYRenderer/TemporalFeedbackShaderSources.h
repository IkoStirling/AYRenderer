#pragma once
#include "AYRenderer/ProceduralNoiseShaderSources.h"
#include <string>
namespace ayt::render {
inline constexpr char kTemporalFeedbackCacheKey[]="temporal_feedback_bounded_tile_hold_v1";
inline const char* temporalFeedbackPhoskiaSource() {
static const std::string source=std::string(kProceduralScreenNoiseFunctions)+R"PHOSKIA(
material TemporalFeedback {
    texture2d currentColor
    texture2d previousColor
    uniform vec4 feedbackParams
    uniform vec4 feedbackGrid
    uniform vec4 feedbackOffset
    vertex {
        in pos:position
        out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5)
        return vec4(pos.xy,0.0,1.0)
    }
    fragment {
        in vUv:texcoord
        let uv=vec2(vUv.x,1.0-vUv.y)
        let current=sampleLod(currentColor,uv,0.0)
        let previousUv=uv
        let weight=feedbackParams.y
        if(feedbackParams.x<0.5) {
            previousUv=uv+feedbackOffset.xy/feedbackGrid.xy
            let inside=step(0.0,previousUv.x)*step(previousUv.x,1.0)*step(0.0,previousUv.y)*step(previousUv.y,1.0)
            weight=weight*inside
        } else {
            weight=1.0
            if(feedbackParams.x<1.5) {
                if(feedbackParams.z<0.5) {weight=0.0}
            } else {
                let cell=vec2(floor(uv.x*feedbackGrid.x/feedbackGrid.z),floor(uv.y*feedbackGrid.y/feedbackGrid.z))
                let phase=floor(ayScreenNoise(cell.x,cell.y,feedbackGrid.w)*feedbackParams.w)
                if(abs(phase-feedbackParams.z)<0.5) {weight=0.0}
            }
        }
        weight=weight*feedbackOffset.z
        let previous=sampleLod(previousColor,previousUv,0.0)
        return mix(current,previous,weight)
    }
}
)PHOSKIA";
return source.c_str();
}
} // namespace ayt::render
