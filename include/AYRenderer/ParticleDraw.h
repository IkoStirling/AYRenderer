#pragma once
#include <cstdint>
#include <vector>
namespace ayt::render {
/// CPU-owned draw payload for a synchronous frame. Quad vertices and packed
/// instances are alternate paths; GPU uploads use transient buffers.
struct ParticleVertex { float x,y,z,u,v,r,g,b,a; };
struct ParticleDrawData {
    std::vector<ParticleVertex> vertices;
    std::vector<uint16_t> indices;
    /// Optional packed vec4 instance stream for a shared mesh. The stride is
    /// a multiple of 16 bytes; when set, the transparent pass binds the mesh
    /// and these instances instead of uploading the CPU quad vertices.
    std::vector<float> instanceData;
    uint16_t instanceStride = 0;
    /// Optional renderer-owned GPU stream. Borrowed through synchronous render;
    /// no GPU handles escape. World 3D streams enter the transparent pass.
    void* gpuStream=nullptr;
    void (*submitGpu)(void*,uint16_t)=nullptr;
    bool world3D=false;
    float sortPosition[3]{};
};
inline constexpr uint16_t kParticleComputeView=243;
inline constexpr const char* kParticlePhoskiaSource = R"(
material ParticleUnlit {
    texture2d albedoMap
    vertex {
        in pos : position
        in uv : texcoord
        in clr : color
        out uvOut : texcoord = uv
        out colorOut : color = clr
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        in colorOut : color
        return sample(albedoMap, uvOut) * colorOut
    }
}
)";
/// Four vec4s per instance: center/radius, camera right, camera up, HDR color.
inline constexpr const char* kEmissivePointInstancedPhoskiaSource = R"(
material EmissivePointInstanced {
    texture2d albedoMap
    vertex {
        in pos : position
        in uv : texcoord
        in centerRadius : instance0
        in rightAxis : instance1
        in upAxis : instance2
        in radiance : instance3
        out uvOut : texcoord = uv
        out colorOut : color = radiance
        let center = centerRadius.xyz
        let offset = rightAxis.xyz * (pos.x * 2.0 * centerRadius.w)
                   + upAxis.xyz * (pos.y * 2.0 * centerRadius.w)
        return modelViewProjection * vec4(center + offset, 1.0)
    }
    fragment {
        in uvOut : texcoord
        in colorOut : color
        return sample(albedoMap, uvOut) * colorOut
    }
}
)";
}
