#pragma once
#include <AYRenderer/PerformanceShaderSources.h>
#include <AYRenderer/PbrShaderSources.h>
#include <string>
#include <string_view>
#include <cctype>
namespace ayt::render::detail {
inline std::string performanceTransparentUniform(std::string_view name){
    for(const auto* property:{"baseColor","metallic","roughness","ao","emissive","opacity","opacitySource","doubleSided","normalYSign","premultipliedAlpha"})if(name==property)return "aypt_"+std::string(name);
    return std::string(name);
}
inline std::string makePerformanceTransparentSource(){
    std::string source=kPbrPhoskiaSource;
    const auto replace=[&](const std::string& from,const std::string& to){const auto at=source.find(from);if(at==source.npos)return false;source.replace(at,from.size(),to);return true;};
    if(!replace("material PBR {","material PerformanceTransparent {\n texture2d patternGobo\n texture2d iesAtlas\n texture2d shadowAtlas\n texture2d contactPosition\n texture2d contactCoverage\n uniform vec4 performanceBlend"))return {};
    const auto uniformsAt=kPerformanceSurfaceSource.find(" uniform vec4 performancePosition");
    const auto uniformsEnd=kPerformanceSurfaceSource.find(" vertex {",uniformsAt);
    const auto materialAt=source.find(" uniform vec4 performanceBlend");
    if(uniformsAt==std::string::npos||uniformsEnd==std::string::npos||materialAt==source.npos)return {};
    source.insert(materialAt,kPerformanceSurfaceSource.substr(uniformsAt,uniformsEnd-uniformsAt));
    const auto opticalAt=kPerformanceSurfaceSource.find("  let delta=position.xyz-performancePosition.xyz");
    const auto opticalEnd=kPerformanceSurfaceSource.find("  let toEye=performanceEye.xyz-position.xyz",opticalAt);
    const auto directAt=source.find("        // Light 0");const auto directEnd=source.find("        let flatAmbient",directAt);
    if(opticalAt==std::string::npos||opticalEnd==std::string::npos||directAt==source.npos||directEnd==source.npos)return {};
    const std::string kernel="        let position=vec4(worldPos,0.0)\n"+kPerformanceSurfaceSource.substr(opticalAt,opticalEnd-opticalAt)+R"(
        let perfView=normalize(performanceEye.xyz-worldPos)
        let perfHalfRaw=perfView+L
        let perfHalf=perfHalfRaw/max(length(perfHalfRaw),0.00001)
        let perfNl=max(dot(N,L),0.0);let perfNv=max(dot(N,perfView),0.0001);let perfNh=max(dot(N,perfHalf),0.0)
        let perfF=materialF0+(vec3(1.0)-materialF0)*pow(1.0-max(dot(perfView,perfHalf),0.0),5.0)
        let perfAlpha=materialRoughness*materialRoughness;let perfAlpha2=perfAlpha*perfAlpha
        let perfDenominator=perfNh*perfNh*(perfAlpha2-1.0)+1.0
        let perfD=perfAlpha2/max(3.14159265*perfDenominator*perfDenominator,0.000001)
        let perfK=(materialRoughness+1.0)*(materialRoughness+1.0)/8.0
        let perfG=(perfNv/(perfNv*(1.0-perfK)+perfK))*(perfNl/max(perfNl*(1.0-perfK)+perfK,0.0001))
        let perfDiffuse=(vec3(1.0)-perfF)*albedo*(1.0-materialMetallic)*0.318309886
        let perfSpecular=perfF*(perfD*perfG/max(4.0*perfNv*perfNl,0.001))
        let direct=(perfDiffuse+perfSpecular)*performanceColor.rgb*perfNl*attenuation*visibility
)";
    source.replace(directAt,directEnd-directAt,kernel);
    const auto ambientAt=source.find("        let flatAmbient");
    const auto alphaAt=source.find("        let alpha =",ambientAt);
    if(ambientAt==source.npos||alphaAt==source.npos)return {};
    source.replace(ambientAt,alphaAt-ambientAt,"        let color=direct\n");
    const auto lightBlock=source.find("uniformblock Lights {");
    const auto pbrMaterial=source.find("material PerformanceTransparent {",lightBlock);
    if(lightBlock==source.npos||pbrMaterial==source.npos)return {};
    source.erase(lightBlock,pbrMaterial-lightBlock);
    for(const auto* line:{"    texture2d aoTexture\n","    texture2d emissiveTexture\n","    texture2d shadowMap\n","    texturecube envCube\n","    texturecube irradianceCube\n","    texturecube prefilteredSpecularCube\n","    texture2d brdfLut\n"})if(!replace(line,""))return {};
    const auto aoAt=source.find("        let materialAo =");
    const auto nvAt=source.find("        let materialF0 =",aoAt);
    if(aoAt==source.npos||nvAt==source.npos)return {};
    source.erase(aoAt,nvAt-aoAt);
    const auto outputAt=source.find("        let outputColor = mix(hazedColor");
    const auto outputEnd=source.find("        return vec4(outputColor, alpha)",outputAt);
    if(outputAt==source.npos||outputEnd==source.npos)return {};
    source.replace(outputAt,outputEnd-outputAt,"        let outputColor=hazedColor*mix(alpha,1.0,performanceBlend.x)\n");
    // bgfx uniform handles are global by name. Isolate replay-only material
    // properties so direct-pass defaults never change the stock PBR's state.
    const auto identifier=[](char c){return std::isalnum(static_cast<unsigned char>(c))||c=='_';};
    for(const auto* name:{"baseColor","metallic","roughness","ao","emissive","opacity","opacitySource","doubleSided","normalYSign","premultipliedAlpha"}){
        const std::string from=name,to=performanceTransparentUniform(name);size_t at=0;
        while((at=source.find(from,at))!=source.npos){const auto end=at+from.size();if((at==0||!identifier(source[at-1]))&&(end==source.size()||!identifier(source[end]))){source.replace(at,from.size(),to);at+=to.size();}else at=end;}
    }
    return std::string(kPerformancePatternSource)+source;
}
inline const std::string& performanceTransparentSource(){static const std::string source=makePerformanceTransparentSource();return source;}
} // namespace ayt::render::detail
