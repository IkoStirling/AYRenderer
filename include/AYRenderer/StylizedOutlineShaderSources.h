#pragma once
namespace ayt::render {
inline constexpr char kStylizedOutlineCacheKey[]="stylized_outline_depth_normal_v1";
inline constexpr char kStylizedOutlinePhoskiaSource[]=R"PHOSKIA(
material StylizedOutline {
    texture2d inputColor
    texture2d outlineDepth
    texture2d outlineNormal
    uniform mat4 outlineInverseProjection
    uniform vec4 outlineParams
    uniform vec4 outlineThresholds
    uniform vec4 outlineAvailable
    uniform vec4 outlineColor
    uniform vec4 styleViewport
    vertex {
        in pos:position
        out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5)
        return vec4(pos.xy,0.0,1.0)
    }
    fragment {
        in vUv:texcoord
        let uv=vec2(vUv.x,1.0-vUv.y)
        let px=min(floor(uv.x*styleViewport.x),styleViewport.x-1.0)
        let py=min(floor(uv.y*styleViewport.y),styleViewport.y-1.0)
        let source=sampleLod(inputColor,uv,0.0)
        let result=source.rgb
        if(outlineParams.x>0.5 && outlineParams.w>0.0) {
            let edge=0.0
            let centerUv=vec2((px+0.5)/styleViewport.x,(py+0.5)/styleViewport.y)
            let centerLuma=dot(sampleLod(inputColor,centerUv,0.0).rgb,vec3(0.2126,0.7152,0.0722))
            let centerDepth=sampleLod(outlineDepth,centerUv,0.0).r
            let centerN=sampleLod(outlineNormal,centerUv,0.0).xyz*2.0-vec3(1.0)
            let centerH=outlineInverseProjection*vec4(centerUv.x*2.0-1.0,1.0-centerUv.y*2.0,mix(centerDepth,centerDepth*2.0-1.0,outlineAvailable.z),1.0)
            let centerZ=abs(centerH.z/max(abs(centerH.w),0.00001))
            for(i in range(4,4)) {
                let direction=vec2(1.0,0.0)
                if(i==1) {direction=vec2(-1.0,0.0)}
                if(i==2) {direction=vec2(0.0,1.0)}
                if(i==3) {direction=vec2(0.0,-1.0)}
                let neighborUv=centerUv+direction*outlineParams.y/styleViewport.xy
                neighborUv=vec2(clamp(neighborUv.x,0.5/styleViewport.x,1.0-0.5/styleViewport.x),clamp(neighborUv.y,0.5/styleViewport.y,1.0-0.5/styleViewport.y))
                if(outlineParams.x<1.5) {
                    let luma=dot(sampleLod(inputColor,neighborUv,0.0).rgb,vec3(0.2126,0.7152,0.0722))
                    edge=max(edge,smoothstep(outlineThresholds.z,outlineThresholds.z*1.5,abs(luma-centerLuma)))
                } else {
                    if(outlineAvailable.x>0.5) {
                        let depth=sampleLod(outlineDepth,neighborUv,0.0).r
                        let neighborH=outlineInverseProjection*vec4(neighborUv.x*2.0-1.0,1.0-neighborUv.y*2.0,mix(depth,depth*2.0-1.0,outlineAvailable.z),1.0)
                        let neighborZ=abs(neighborH.z/max(abs(neighborH.w),0.00001))
                        let covered=1.0-step(0.999999,centerDepth)
                        let otherCovered=1.0-step(0.999999,depth)
                        let discontinuity=abs(centerZ-neighborZ)/max(min(centerZ,neighborZ),0.01)
                        let crease=smoothstep(outlineThresholds.x,outlineThresholds.x*1.5,discontinuity)*covered*otherCovered
                        edge=max(edge,max(abs(covered-otherCovered),crease))
                        if(outlineParams.x>2.5 && outlineAvailable.y>0.5) {
                            let n=sampleLod(outlineNormal,neighborUv,0.0).xyz*2.0-vec3(1.0)
                            let normalDifference=1.0-clamp(dot(n,centerN)/max(length(n)*length(centerN),0.00001),-1.0,1.0)
                            edge=max(edge,smoothstep(outlineThresholds.y,min(2.001,outlineThresholds.y*1.5),normalDifference)*covered*otherCovered)
                        }
                    }
                }
            }
            result=mix(result,outlineColor.rgb,edge*outlineParams.w)
        }
        return vec4(result,source.a)
    }
}
)PHOSKIA";
} // namespace ayt::render
