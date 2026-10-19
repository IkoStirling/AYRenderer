#pragma once

namespace ayt::render {
inline constexpr char kColorStylizationCacheKey[] = "color_stylization_bayer4_pixel_center_v1";
inline constexpr char kColorStylizationPhoskiaSource[] = R"PHOSKIA(
material ColorStylization {
    texture2d inputColor
    uniform vec4 styleParams
    uniform vec4 styleViewport
    vertex {
        in pos : position
        out vUv : texcoord = pos.xy * vec2(0.5) + vec2(0.5)
        return vec4(pos.xy, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let uv = vec2(vUv.x, 1.0-vUv.y)
        let px = min(floor(uv.x*styleViewport.x), styleViewport.x-1.0)
        let py = min(floor(uv.y*styleViewport.y), styleViewport.y-1.0)
        let block = styleParams.x
        let bx = floor(px/block)
        let by = floor(py/block)
        let firstX = bx*block
        let firstY = by*block
        // Select one real texel center, including partial blocks at the edge.
        // This remains a nearest sample even when the input target is linear.
        let extentX = min(block, styleViewport.x-firstX)
        let extentY = min(block, styleViewport.y-firstY)
        let sampleX = firstX+floor((extentX-1.0)*0.5)+0.5
        let sampleY = firstY+floor((extentY-1.0)*0.5)+0.5
        let source = sampleLod(inputColor,vec2(sampleX/styleViewport.x,sampleY/styleViewport.y),0.0)
        // Analytic Bayer4 ranks: rows 0,8,2,10 / 12,4,14,6 /
        // 3,11,1,9 / 15,7,13,5. Anchored to viewport virtual pixels.
        let x0 = bx-floor(bx*0.5)*2.0
        let y0 = by-floor(by*0.5)*2.0
        let x1 = floor(bx*0.5)-floor(bx*0.25)*2.0
        let y1 = floor(by*0.5)-floor(by*0.25)*2.0
        let rank = 4.0*(2.0*x0+3.0*y0-4.0*x0*y0)+(2.0*x1+3.0*y1-4.0*x1*y1)
        let offset = ((rank+0.5)/16.0-0.5)*styleParams.z
        let steps = styleParams.y-1.0
        let r = floor(clamp(source.r+offset/steps,0.0,1.0)*steps+0.5)/steps
        let g = floor(clamp(source.g+offset/steps,0.0,1.0)*steps+0.5)/steps
        let b = floor(clamp(source.b+offset/steps,0.0,1.0)*steps+0.5)/steps
        return vec4(mix(source.rgb,vec3(r,g,b),styleParams.w),source.a)
    }
}
)PHOSKIA";
} // namespace ayt::render
