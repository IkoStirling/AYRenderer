#pragma once
#include <cstdint>
#include <vector>
namespace ayt::render {
/// CPU-owned, packed position/UV/linear-color vertices for a synchronous frame.
/// Each batch contains at most 16383 quads. GPU uploads use transient buffers.
struct ParticleVertex { float x,y,z,u,v,r,g,b,a; };
struct ParticleDrawData {
    std::vector<ParticleVertex> vertices;
    std::vector<uint16_t> indices;
    /// Optional renderer-owned GPU stream. Borrowed through synchronous render;
    /// no GPU handles escape. Submission runs in the globally sorted overlay lane.
    void* gpuStream=nullptr;
    void (*submitGpu)(void*,uint16_t)=nullptr;
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
}
