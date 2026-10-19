#pragma once

namespace ayt::render {
inline constexpr char kColorStylizationCacheKey[] = "color_stylization_value_bands_v3";
inline constexpr char kColorStylizationPhoskiaSource[] = R"PHOSKIA(
material ColorStylization {
    texture2d inputColor
    uniform vec4 styleParams
    uniform vec4 styleViewport
    uniform vec4 mappingParams
    uniform vec4 shadowColor
    uniform vec4 highlightColor
    uniform vec4 paletteColors[16]
    uniform vec4 printParams
    uniform vec4 printRotation
    uniform vec4 inkColor
    uniform vec4 paperColor
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
        let result = mix(source.rgb,vec3(r,g,b),styleParams.w)
        if(mappingParams.x>0.5 && mappingParams.x<1.5) {
            let luminance=dot(source.rgb,vec3(0.2126,0.7152,0.0722))
            let tone=clamp(luminance+offset/steps,0.0,1.0)
            tone=mix(tone,floor(tone*steps+0.5)/steps,styleParams.w)
            result=mix(shadowColor.rgb,highlightColor.rgb,tone)
        }
        if(mappingParams.x>1.5 && mappingParams.x<2.5) {
            let candidate=vec3(clamp(source.r+offset/8.0,0.0,1.0),clamp(source.g+offset/8.0,0.0,1.0),clamp(source.b+offset/8.0,0.0,1.0))
            let best=1000.0
            for(i in range(int(mappingParams.y),16)) {
                let delta=candidate-paletteColors[i].rgb
                let distance=dot(delta*delta,vec3(0.2126,0.7152,0.0722))
                if(distance<best) {best=distance;result=paletteColors[i].rgb}
            }
        }
        if(mappingParams.x>2.5) {
            let value=max(source.r,max(source.g,source.b))
            let quantized=floor(clamp(value+offset/steps,0.0,1.0)*steps+0.5)/steps
            let target=mix(value,quantized,styleParams.w)
            result=source.rgb*(target/max(value,0.000001))
        }
        // A continuous screen in physical viewport coordinates. Quantization
        // controls the source tone; pattern spacing is independent of pixelation.
        if(printParams.x>0.5 && printParams.w>0.0) {
            let tone=pow(clamp(dot(result,vec3(0.2126,0.7152,0.0722)),0.0,1.0),printParams.z)
            let printPoint=vec2(px+0.5,py+0.5)
            let rotated=vec2(dot(printPoint,printRotation.xy),dot(printPoint,printRotation.zw))/printParams.y
            let cell=rotated-floor(rotated)-vec2(0.5)
            let aa=0.7/printParams.y
            let ink=0.0
            if(printParams.x<1.5) {
                // Circle radius grows with darkness; full black fills the cell.
                let radius=sqrt((1.0-tone)*0.318309886)
                ink=(1.0-smoothstep(radius-aa,radius+aa,length(cell)))*(1.0-step(0.9999,tone))
            } else {
                let coverage=1.0-tone
                let width=coverage*0.5
                ink=1.0-smoothstep(width-aa,width+aa,abs(cell.y))
                if(printParams.x>2.5) {
                    let crossWidth=clamp((coverage-0.45)/0.55,0.0,1.0)*0.5
                    let cross=(1.0-smoothstep(crossWidth-aa,crossWidth+aa,abs(cell.x)))*step(0.45001,coverage)
                    ink=max(ink,cross)
                }
                ink=ink*(1.0-step(0.9999,tone))
            }
            ink=max(ink,1.0-step(0.0001,tone))
            result=mix(result,mix(paperColor.rgb,inkColor.rgb,ink),printParams.w)
        }
        return vec4(result,source.a)
    }
}
)PHOSKIA";
} // namespace ayt::render
